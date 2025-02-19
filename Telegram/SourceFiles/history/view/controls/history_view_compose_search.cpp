/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "history/view/controls/history_view_compose_search.h"

#include "api/api_messages_search.h"
#include "boxes/peer_list_box.h"
#include "data/data_session.h"
#include "dialogs/dialogs_search_from_controllers.h" // SearchFromBox
#include "dialogs/ui/dialogs_layout.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "ui/effects/show_animation.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/multi_select.h"
#include "ui/widgets/scroll_area.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_chat.h"
#include "styles/style_dialogs.h"
#include "styles/style_info.h"

namespace HistoryView {
namespace {

[[nodiscard]] inline bool HasChooseFrom(not_null<History*> history) {
	if (const auto peer = history->peer) {
		return (peer->isChat() || peer->isMegagroup());
	}
	return false;
}

struct SearchRequest {
	QString query;
	PeerData *from = nullptr;
};

class Row final : public PeerListRow {
public:
	Row(std::unique_ptr<Dialogs::FakeRow> fakeRow);

	[[nodiscard]] FullMsgId fullId() const;

	QRect elementGeometry(int element, int outerWidth) const override;
	void elementAddRipple(
		int element,
		QPoint point,
		Fn<void()> updateCallback) override;
	void elementsStopLastRipple() override;
	void elementsPaint(
		Painter &p,
		int outerWidth,
		bool selected,
		int selectedElement) override;

private:
	const std::unique_ptr<Dialogs::FakeRow> _fakeRow;

	int _outerWidth = 0;

};

Row::Row(std::unique_ptr<Dialogs::FakeRow> fakeRow)
: PeerListRow(
	fakeRow->searchInChat().history()->peer,
	fakeRow->item()->fullId().msg.bare)
, _fakeRow(std::move(fakeRow)) {
}

FullMsgId Row::fullId() const {
	return _fakeRow->item()->fullId();
}

QRect Row::elementGeometry(int element, int outerWidth) const {
	return QRect(0, 0, outerWidth, st::dialogsRowHeight);
}

void Row::elementAddRipple(
		int element,
		QPoint point,
		Fn<void()> updateCallback) {
	_fakeRow->addRipple(
		point,
		{ _outerWidth, st::dialogsRowHeight },
		std::move(updateCallback));
}

void Row::elementsStopLastRipple() {
	_fakeRow->stopLastRipple();
}

void Row::elementsPaint(
		Painter &p,
		int outerWidth,
		bool selected,
		int selectedElement) {
	_outerWidth = outerWidth;
	using Row = Dialogs::Ui::RowPainter;
	Row::paint(p, _fakeRow.get(), outerWidth, false, selected, 0, false);
}

class ListController final : public PeerListController {
public:
	explicit ListController(not_null<History*> history);

	Main::Session &session() const override;
	void prepare() override;
	void rowClicked(not_null<PeerListRow*> row) override;
	void rowElementClicked(not_null<PeerListRow*> row, int element) override;

	void loadMoreRows() override;

	void addItems(const MessageIdsList &ids, bool clear);

	[[nodiscard]] rpl::producer<FullMsgId> showItemRequests() const;
	[[nodiscard]] rpl::producer<> searchMoreRequests() const;
	[[nodiscard]] rpl::producer<> resetScrollRequests() const;

private:
	const not_null<History*> _history;
	rpl::event_stream<FullMsgId> _showItemRequests;
	rpl::event_stream<> _searchMoreRequests;
	rpl::event_stream<> _resetScrollRequests;

};

ListController::ListController(not_null<History*> history)
: _history(history) {
}

Main::Session &ListController::session() const {
	return _history->owner().session();
}

void ListController::prepare() {
}

void ListController::rowClicked(not_null<PeerListRow*> row) {
	_showItemRequests.fire_copy(static_cast<Row*>(row.get())->fullId());
}

void ListController::rowElementClicked(
		not_null<PeerListRow*> row,
		int element) {
	ListController::rowClicked(row);
}

void ListController::loadMoreRows() {
	_searchMoreRequests.fire({});
}

rpl::producer<FullMsgId> ListController::showItemRequests() const {
	return _showItemRequests.events();
}

rpl::producer<> ListController::searchMoreRequests() const {
	return _searchMoreRequests.events();
}

rpl::producer<> ListController::resetScrollRequests() const {
	return _resetScrollRequests.events();
}

void ListController::addItems(const MessageIdsList &ids, bool clear) {
	if (clear) {
		_resetScrollRequests.fire({});
		for (auto i = 0; i != delegate()->peerListFullRowsCount();) {
			delegate()->peerListRemoveRow(delegate()->peerListRowAt(i));
		}
	}

	const auto &owner = _history->owner();
	const auto key = Dialogs::Key{ _history };
	for (const auto &id : ids) {
		if (const auto item = owner.message(id)) {
			delegate()->peerListAppendRow(std::make_unique<Row>(
				std::make_unique<Dialogs::FakeRow>(key, item)));
		}
	}

	delegate()->peerListRefreshRows();

	if (!delegate()->peerListFullRowsCount()) {
		_showItemRequests.fire({});
	}
}

struct List {
	base::unique_qptr<Ui::RpWidget> container;
	std::unique_ptr<ListController> controller;
};

List CreateList(
		not_null<Ui::RpWidget*> parent,
		not_null<History*> history) {
	auto list = List{
		base::make_unique_q<Ui::RpWidget>(parent),
		std::make_unique<ListController>(history),
	};
	const auto scroll = Ui::CreateChild<Ui::ScrollArea>(list.container.get());

	using Delegate = PeerListContentDelegateSimple;
	const auto delegate = scroll->lifetime().make_state<Delegate>();
	list.controller->setStyleOverrides(&st::searchInChatPeerList);

	const auto content = scroll->setOwnedWidget(
		object_ptr<PeerListContent>(scroll, list.controller.get()));

	list.controller->resetScrollRequests(
	) | rpl::start_with_next([=] {
		scroll->scrollToY(0);
	}, scroll->lifetime());

	scroll->scrolls(
	) | rpl::start_with_next([=] {
		const auto top = scroll->scrollTop();
		content->setVisibleTopBottom(top, top + scroll->height());
	}, scroll->lifetime());

	delegate->setContent(content);
	list.controller->setDelegate(delegate);

	list.container->sizeValue(
	) | rpl::start_with_next([=](const QSize &size) {
		content->resize(size.width(), content->height());
		scroll->resize(size);
	}, list.container->lifetime());

	list.container->paintRequest(
	) | rpl::start_with_next([weak = Ui::MakeWeak(list.container.get())](
			const QRect &r) {
		Painter p(weak);

		p.fillRect(r, st::dialogsBg);
	}, list.container->lifetime());

	return list;
}

class TopBar final : public Ui::RpWidget {
public:
	TopBar(not_null<Ui::RpWidget*> parent);

	void setInnerFocus();

	[[nodiscard]] PeerData *from() const;

	[[nodiscard]] rpl::producer<SearchRequest> searchRequests() const;
	[[nodiscard]] rpl::producer<PeerData*> fromValue() const;
	[[nodiscard]] rpl::producer<> queryChanges() const;
	[[nodiscard]] rpl::producer<> closeRequests() const;

	void setFrom(PeerData *peer);

private:
	void clearItems();
	void requestSearch(bool cache = true);
	void requestSearchDelayed();

	base::unique_qptr<Ui::IconButton> _cancel;
	base::unique_qptr<Ui::MultiSelect> _select;

	rpl::variable<PeerData*> _from = nullptr;;

	base::Timer _searchTimer;

	std::vector<SearchRequest> _typedRequests;

	rpl::event_stream<SearchRequest> _searchRequests;
	rpl::event_stream<> _queryChanges;
};

TopBar::TopBar(not_null<Ui::RpWidget*> parent)
: Ui::RpWidget(parent)
, _cancel(base::make_unique_q<Ui::IconButton>(this, st::historyTopBarBack))
, _select(base::make_unique_q<Ui::MultiSelect>(
	this,
	st::searchInChatMultiSelect,
	tr::lng_dlg_filter()))
, _searchTimer([=] { requestSearch(); }) {

	parent->geometryValue(
	) | rpl::start_with_next([=](const QRect &r) {
		moveToLeft(0, 0);
		resize(r.width(), st::topBarHeight);
	}, lifetime());

	sizeValue(
	) | rpl::start_with_next([=](const QSize &s) {
		_cancel->moveToLeft(0, (s.height() - _cancel->height()) / 2);

		const auto selectLeft = _cancel->x() + _cancel->width();
		_select->resizeToWidth(s.width() - selectLeft);
		_select->moveToLeft(selectLeft, (s.height() - _select->height()) / 2);

	}, lifetime());

	paintRequest(
	) | rpl::start_with_next([=](const QRect &r) {
		Painter p(this);

		p.fillRect(r, st::dialogsBg);
	}, lifetime());

	_select->setQueryChangedCallback([=](const QString &) {
		requestSearchDelayed();
		_queryChanges.fire({});
	});

	_select->setSubmittedCallback([=](Qt::KeyboardModifiers) {
		requestSearch();
	});

	_select->setCancelledCallback([=] {

	});
}

void TopBar::setInnerFocus() {
	_select->setInnerFocus();
}

void TopBar::clearItems() {
	_select->setItemRemovedCallback(nullptr);

	for (const auto &id : _select->getItems()) {
		_select->removeItem(id);
	}

	_select->setItemRemovedCallback([=](uint64) {
		_from = nullptr;
		requestSearchDelayed();
	});
}

void TopBar::requestSearch(bool cache) {
	const auto search = SearchRequest{ _select->getQuery(), _from.current() };
	if (cache) {
		_typedRequests.push_back(search);
	}
	_searchRequests.fire_copy(search);
}

void TopBar::requestSearchDelayed() {
	// Check cached queries.
	for (const auto &t : _typedRequests) {
		if (t.query == _select->getQuery() && t.from == _from.current()) {
			requestSearch(false);
			return;
		}
	}

	_searchTimer.callOnce(AutoSearchTimeout);
}

rpl::producer<SearchRequest> TopBar::searchRequests() const {
	return _searchRequests.events();
}

rpl::producer<> TopBar::queryChanges() const {
	return _queryChanges.events();
}

rpl::producer<> TopBar::closeRequests() const {
	return _cancel->clicks() | rpl::to_empty;
}

rpl::producer<PeerData*> TopBar::fromValue() const {
	return _from.value();
}

PeerData *TopBar::from() const {
	return _from.current();
}

void TopBar::setFrom(PeerData *peer) {
	clearItems();

	const auto guard = gsl::finally([&] {
		_from = peer;
		requestSearchDelayed();
	});
	if (!peer) {
		return;
	}

	_select->addItem(
		peer->id.value,
		tr::lng_dlg_search_from(tr::now, lt_user, peer->shortName()),
		st::activeButtonBg,
		PaintUserpicCallback(peer, false),
		Ui::MultiSelect::AddItemWay::Default);
}

class BottomBar final : public Ui::RpWidget {
public:
	using Index = int;
	BottomBar(not_null<Ui::RpWidget*> parent, bool fastShowChooseFrom);

	void setTotal(int total);
	void setCurrent(int current);

	[[nodiscard]] rpl::producer<Index> showItemRequests() const;
	[[nodiscard]] rpl::producer<> showCalendarRequests() const;
	[[nodiscard]] rpl::producer<> showBoxFromRequests() const;
	[[nodiscard]] rpl::producer<> showListRequests() const;

	void buttonFromToggleOn(rpl::producer<bool> &&visible);
	void buttonCalendarToggleOn(rpl::producer<bool> &&visible);

private:
	void updateText(int current);

	base::unique_qptr<Ui::FlatButton> _showList;

	base::unique_qptr<Ui::IconButton> _previous;
	base::unique_qptr<Ui::IconButton> _next;

	base::unique_qptr<Ui::IconButton> _jumpToDate;
	base::unique_qptr<Ui::IconButton> _chooseFromUser;
	base::unique_qptr<Ui::FlatLabel> _counter;

	int _total = -1;
	rpl::variable<int> _current = 0;
};

BottomBar::BottomBar(not_null<Ui::RpWidget*> parent, bool fastShowChooseFrom)
: Ui::RpWidget(parent)
, _showList(base::make_unique_q<Ui::FlatButton>(
	this,
	QString(),
	st::historyComposeButton))
// Icons are swaped.
, _previous(base::make_unique_q<Ui::IconButton>(this, st::calendarNext))
, _next(base::make_unique_q<Ui::IconButton>(this, st::calendarPrevious))
, _jumpToDate(base::make_unique_q<Ui::IconButton>(this, st::dialogCalendar))
, _chooseFromUser(
	base::make_unique_q<Ui::IconButton>(this, st::dialogSearchFrom))
, _counter(base::make_unique_q<Ui::FlatLabel>(
	this,
	st::defaultSettingsRightLabel)) {

	_counter->setAttribute(Qt::WA_TransparentForMouseEvents);
	_chooseFromUser->setVisible(fastShowChooseFrom);

	parent->geometryValue(
	) | rpl::start_with_next([=](const QRect &r) {
		const auto height = st::historyComposeButton.height;
		resize(r.width(), height);
		moveToLeft(0, r.height() - height);
	}, lifetime());

	auto mapSize = rpl::map([=] { return size(); });
	rpl::merge(
		_jumpToDate->shownValue() | mapSize,
		_chooseFromUser->shownValue() | mapSize,
		_counter->sizeValue() | mapSize,
		sizeValue()
	) | rpl::start_with_next([=](const QSize &s) {
		_showList->setGeometry(QRect(QPoint(), s));
		_previous->moveToRight(0, (s.height() - _previous->height()) / 2);
		_next->moveToRight(
			_previous->width(),
			(s.height() - _next->height()) / 2);

		auto left = st::topBarActionSkip;
		const auto list = std::vector<not_null<Ui::RpWidget*>>{
			_jumpToDate.get(),
			_chooseFromUser.get(),
			_counter.get() };
		for (const auto &w : list) {
			if (w->isHidden()) {
				continue;
			}
			w->moveToLeft(left, (s.height() - w->height()) / 2);
			left += w->width();
		}
	}, lifetime());

	paintRequest(
	) | rpl::start_with_next([=](const QRect &r) {
		Painter p(this);

		p.fillRect(r, st::dialogsBg);
	}, lifetime());

	_current.value(
	) | rpl::start_with_next([=](int current) {
		const auto nextDisabled = (current <= 0) || (current >= _total);
		const auto prevDisabled = (current <= 1);
		_next->setAttribute(Qt::WA_TransparentForMouseEvents, nextDisabled);
		_previous->setAttribute(
			Qt::WA_TransparentForMouseEvents,
			prevDisabled);
		_next->setIconOverride(nextDisabled
			? &st::calendarPreviousDisabled
			: nullptr);
		_previous->setIconOverride(prevDisabled
			? &st::calendarNextDisabled
			: nullptr);

		_showList->setAttribute(
			Qt::WA_TransparentForMouseEvents,
			nextDisabled && prevDisabled);
		updateText(current);
	}, lifetime());

	rpl::merge(
		_next->clicks() | rpl::map_to(1),
		_previous->clicks() | rpl::map_to(-1)
	) | rpl::start_with_next([=](int way) {
		_current = _current.current() + way;
	}, lifetime());
}

void BottomBar::setTotal(int total) {
	_total = total;
	setCurrent(1);
}

void BottomBar::setCurrent(int current) {
	_current.force_assign(current);
}

void BottomBar::updateText(int current) {
	if (_total < 0) {
		_counter->setText(QString());
	} else if (_total) {
		_counter->setText(tr::lng_search_messages_n_of_amount(
			tr::now,
			lt_n,
			QString::number(current),
			lt_amount,
			QString::number(_total)));
	} else {
		_counter->setText(tr::lng_search_messages_none(tr::now));
	}
}

rpl::producer<BottomBar::Index> BottomBar::showItemRequests() const {
	return _current.changes() | rpl::map(rpl::mappers::_1 - 1);
}

rpl::producer<> BottomBar::showCalendarRequests() const {
	return _jumpToDate->clicks() | rpl::to_empty;
}

rpl::producer<> BottomBar::showBoxFromRequests() const {
	return _chooseFromUser->clicks() | rpl::to_empty;
}

rpl::producer<> BottomBar::showListRequests() const {
	return _showList->clicks() | rpl::to_empty;
}

void BottomBar::buttonFromToggleOn(rpl::producer<bool> &&visible) {
	std::move(
		visible
	) | rpl::start_with_next([=](bool value) {
		_chooseFromUser->setVisible(value);
	}, _chooseFromUser->lifetime());
}

void BottomBar::buttonCalendarToggleOn(rpl::producer<bool> &&visible) {
	std::move(
		visible
	) | rpl::start_with_next([=](bool value) {
		_jumpToDate->setVisible(value);
	}, _jumpToDate->lifetime());
}

class ApiSearch final {
public:
	ApiSearch(not_null<Main::Session*> session, not_null<History*> history);

	void clear();
	void search(const SearchRequest &search);
	void searchMore();

	const Api::FoundMessages &messages() const;

	[[nodiscard]] rpl::producer<> newFounds() const;
	[[nodiscard]] rpl::producer<> nextFounds() const;

private:
	void addFound(const Api::FoundMessages &data);

	Api::MessagesSearch _apiSearch;

	std::optional<Api::MessagesSearch> _migratedSearch;
	Api::FoundMessages _migratedFirstFound;

	Api::FoundMessages _concatedFound;

	bool _waitingForTotal = false;
	bool _isFull = false;

	rpl::event_stream<> _newFounds;
	rpl::event_stream<> _nextFounds;

	rpl::lifetime _lifetime;

};

ApiSearch::ApiSearch(
	not_null<Main::Session*> session,
	not_null<History*> history)
: _apiSearch(session, history)
, _migratedSearch(history->migrateFrom()
	? std::make_optional<Api::MessagesSearch>(session, history->migrateFrom())
	: std::nullopt) {

	const auto checkWaitingForTotal = [=] {
		if (_waitingForTotal) {
			if (_concatedFound.total >= 0 && _migratedFirstFound.total >= 0) {
				_waitingForTotal = false;
				_concatedFound.total += _migratedFirstFound.total;
				_newFounds.fire({});
			}
		} else {
			_newFounds.fire({});
		}
	};

	const auto checkFull = [=](const Api::FoundMessages &data) {
		if (data.total == int(_concatedFound.messages.size())) {
			_isFull = true;
			addFound(_migratedFirstFound);
		}
	};

	_apiSearch.messagesFounds(
	) | rpl::start_with_next([=](const Api::FoundMessages &data) {
		if (data.nextToken == _concatedFound.nextToken) {
			addFound(data);
			checkFull(data);
			_nextFounds.fire({});
		} else {
			_concatedFound = data;
			checkFull(data);
			checkWaitingForTotal();
		}
	}, _lifetime);

	if (_migratedSearch) {
		_migratedSearch->messagesFounds(
		) | rpl::start_with_next([=](const Api::FoundMessages &data) {
			if (_isFull) {
				addFound(data);
			}
			if (data.nextToken == _migratedFirstFound.nextToken) {
				_nextFounds.fire({});
			} else {
				_migratedFirstFound = data;
				checkWaitingForTotal();
			}
		}, _lifetime);
	}
}

void ApiSearch::addFound(const Api::FoundMessages &data) {
	for (const auto &message : data.messages) {
		_concatedFound.messages.push_back(message);
	}
}

const Api::FoundMessages &ApiSearch::messages() const {
	return _concatedFound;
}

void ApiSearch::clear() {
	_concatedFound = {};
	_migratedFirstFound = {};
}

void ApiSearch::search(const SearchRequest &search) {
	if (_migratedSearch) {
		_waitingForTotal = true;
		_migratedSearch->searchMessages(search.query, search.from);
	}
	_apiSearch.searchMessages(search.query, search.from);
}

void ApiSearch::searchMore() {
	if (_migratedSearch && _isFull) {
		_migratedSearch->searchMore();
	} else {
		_apiSearch.searchMore();
	}
}

rpl::producer<> ApiSearch::newFounds() const {
	return _newFounds.events();
}

rpl::producer<> ApiSearch::nextFounds() const {
	return _nextFounds.events();
}

} // namespace

class ComposeSearch::Inner final {
public:
	Inner(
		not_null<Ui::RpWidget*> parent,
		not_null<Window::SessionController*> window,
		not_null<History*> history);
	~Inner();

	void hideAnimated();
	void setInnerFocus();

	[[nodiscard]] rpl::producer<> destroyRequests() const;
	[[nodiscard]] rpl::lifetime &lifetime();

private:
	void showAnimated();
	void hideList();

	const not_null<Window::SessionController*> _window;
	const not_null<History*> _history;
	const base::unique_qptr<TopBar> _topBar;
	const base::unique_qptr<BottomBar> _bottomBar;
	const List _list;

	ApiSearch _apiSearch;

	struct {
		struct {
			QString token;
			BottomBar::Index index = -1;
		} data;
		rpl::event_stream<BottomBar::Index> jumps;
	} _pendingJump;

	rpl::event_stream<> _destroyRequests;

};

ComposeSearch::Inner::Inner(
	not_null<Ui::RpWidget*> parent,
	not_null<Window::SessionController*> window,
	not_null<History*> history)
: _window(window)
, _history(history)
, _topBar(base::make_unique_q<TopBar>(parent))
, _bottomBar(base::make_unique_q<BottomBar>(parent, HasChooseFrom(history)))
, _list(CreateList(parent, history))
, _apiSearch(&window->session(), history) {
	showAnimated();

	rpl::combine(
		_topBar->geometryValue(),
		_bottomBar->geometryValue()
	) | rpl::start_with_next([=](const QRect &top, const QRect &bottom) {
		_list.container->setGeometry(QRect(
			top.topLeft() + QPoint(0, top.height()),
			bottom.topLeft() + QPoint(bottom.width(), 0)));
	}, _list.container->lifetime());

	_topBar->searchRequests(
	) | rpl::start_with_next([=](const SearchRequest &search) {
		if (search.query.isEmpty() && !search.from) {
			return;
		}
		_apiSearch.clear();
		_apiSearch.search(search);
	}, _topBar->lifetime());

	_topBar->queryChanges(
	) | rpl::start_with_next([=] {
		hideList();
	}, _topBar->lifetime());

	_topBar->closeRequests(
	) | rpl::start_with_next([=] {
		hideAnimated();
	}, _topBar->lifetime());

	_apiSearch.newFounds(
	) | rpl::start_with_next([=] {
		const auto &apiData = _apiSearch.messages();
		_bottomBar->setTotal(apiData.total);
		_list.controller->addItems(apiData.messages, true);
	}, _topBar->lifetime());

	_apiSearch.nextFounds(
	) | rpl::start_with_next([=] {
		if (_pendingJump.data.token == _apiSearch.messages().nextToken) {
			_pendingJump.jumps.fire_copy(_pendingJump.data.index);
		}
		_list.controller->addItems(_apiSearch.messages().messages, false);
	}, _topBar->lifetime());

	const auto goToMessage = [=](const FullMsgId &itemId) {
		const auto item = _history->owner().message(itemId);
		if (item) {
			_window->jumpToChatListEntry({
				{ item->history() },
				item->fullId(),
			});
		}
	};

	rpl::merge(
		_pendingJump.jumps.events() | rpl::filter(rpl::mappers::_1 >= 0),
		_bottomBar->showItemRequests()
	) | rpl::start_with_next([=](BottomBar::Index index) {
		const auto &apiData = _apiSearch.messages();
		const auto &messages = apiData.messages;
		const auto size = int(messages.size());
		if (index >= (size - 1) && size != apiData.total) {
			_apiSearch.searchMore();
		}
		if (index >= size || index < 0) {
			_pendingJump.data = { apiData.nextToken, index };
			return;
		}
		_pendingJump.data = {};
		goToMessage(messages[index]);
		hideList();
	}, _bottomBar->lifetime());

	_list.controller->showItemRequests(
	) | rpl::start_with_next([=](const FullMsgId &id) {
		const auto &messages = _apiSearch.messages().messages;
		const auto it = ranges::find(messages, id);
		if (it != end(messages)) {
			_bottomBar->setCurrent(std::distance(begin(messages), it) + 1);
		}
	}, _list.container->lifetime());

	_list.controller->searchMoreRequests(
	) | rpl::start_with_next([=] {
		const auto &apiData = _apiSearch.messages();
		if (int(apiData.messages.size()) != apiData.total) {
			_apiSearch.searchMore();
		}
	}, _list.container->lifetime());

	_bottomBar->showCalendarRequests(
	) | rpl::start_with_next([=] {
		hideList();
		_window->showCalendar({ _history }, QDate());
	}, _bottomBar->lifetime());

	_bottomBar->showBoxFromRequests(
	) | rpl::start_with_next([=] {
		const auto peer = _history->peer;
		auto box = Dialogs::SearchFromBox(
			peer,
			crl::guard(_bottomBar.get(), [=](not_null<PeerData*> from) {
				Window::Show(_window).hideLayer();
				_topBar->setFrom(from);
			}),
			crl::guard(_bottomBar.get(), [=] { /*_filter->setFocus();*/ }));

		Window::Show(_window).showBox(std::move(box));
	}, _bottomBar->lifetime());

	_bottomBar->showListRequests(
	) | rpl::start_with_next([=] {
		if (_list.container->isHidden()) {
			Ui::Animations::ShowWidgets({ _list.container.get() });
		} else {
			hideList();
		}
	}, _bottomBar->lifetime());

	_bottomBar->buttonCalendarToggleOn(_topBar->fromValue(
	) | rpl::map([=](PeerData *from) {
		return !from;
	}));

	_bottomBar->buttonFromToggleOn(_topBar->fromValue(
	) | rpl::map([=](PeerData *from) {
		return HasChooseFrom(_history) && !from;
	}));
}

void ComposeSearch::Inner::setInnerFocus() {
	_topBar->setInnerFocus();
}

void ComposeSearch::Inner::showAnimated() {
	// Don't animate bottom bar.
	_bottomBar->show();
	Ui::Animations::ShowWidgets({ _topBar.get() });
}

void ComposeSearch::Inner::hideAnimated() {
	hideList();
	Ui::Animations::HideWidgets({ _topBar.get(), _bottomBar.get() });

	_destroyRequests.fire({});
}

void ComposeSearch::Inner::hideList() {
	if (!_list.container->isHidden()) {
		Ui::Animations::HideWidgets({ _list.container.get() });
	}
}

rpl::producer<> ComposeSearch::Inner::destroyRequests() const {
	return _destroyRequests.events();
}

rpl::lifetime &ComposeSearch::Inner::lifetime() {
	return _topBar->lifetime();
}

ComposeSearch::Inner::~Inner() {
}

ComposeSearch::ComposeSearch(
	not_null<Ui::RpWidget*> parent,
	not_null<Window::SessionController*> window,
	not_null<History*> history)
: _inner(std::make_unique<Inner>(parent, window, history)) {
}

ComposeSearch::~ComposeSearch() {
}

void ComposeSearch::hideAnimated() {
	_inner->hideAnimated();
}

void ComposeSearch::setInnerFocus() {
	_inner->setInnerFocus();
}

rpl::producer<> ComposeSearch::destroyRequests() const {
	return _inner->destroyRequests();
}

rpl::lifetime &ComposeSearch::lifetime() {
	return _inner->lifetime();
}

} // namespace HistoryView
