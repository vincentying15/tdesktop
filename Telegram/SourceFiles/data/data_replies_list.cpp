/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/data_replies_list.h"

#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "data/data_histories.h"
#include "data/data_session.h"
#include "data/data_changes.h"
#include "data/data_channel.h"
#include "data/data_messages.h"
#include "apiwrap.h"

namespace Data {
namespace {

constexpr auto kMessagesPerPage = 4; // #TODO replies

} // namespace

struct RepliesList::Viewer {
	MessagesSlice slice;
	MsgId around = 0;
	int limitBefore = 0;
	int limitAfter = 0;
};

RepliesList::RepliesList(not_null<History*> history, MsgId rootId)
: _history(history)
, _rootId(rootId) {
}

RepliesList::~RepliesList() {
	histories().cancelRequest(base::take(_beforeId));
	histories().cancelRequest(base::take(_afterId));
}

rpl::producer<MessagesSlice> RepliesList::source(
		MessagePosition aroundId,
		int limitBefore,
		int limitAfter) {
	const auto around = aroundId.fullId.msg;
	return [=](auto consumer) {
		auto lifetime = rpl::lifetime();
		const auto viewer = lifetime.make_state<Viewer>();
		const auto push = [=] {
			if (buildFromData(viewer)) {
				consumer.put_next_copy(viewer->slice);
			}
		};
		viewer->around = around;
		viewer->limitBefore = limitBefore;
		viewer->limitAfter = limitAfter;

		_history->session().changes().messageUpdates(
			MessageUpdate::Flag::NewAdded
			| MessageUpdate::Flag::NewMaybeAdded
			| MessageUpdate::Flag::Destroyed
		) | rpl::filter([=](const MessageUpdate &update) {
			return applyUpdate(viewer, update);
		}) | rpl::start_with_next([=] {
			crl::on_main(this, push);
		}, lifetime);

		_partLoaded.events(
		) | rpl::start_with_next([=] {
			crl::on_main(this, push);
		}, lifetime);

		push();
		return lifetime;
	};
}

rpl::producer<int> RepliesList::fullCount() const {
	return _fullCount.value() | rpl::filter_optional();
}

bool RepliesList::buildFromData(not_null<Viewer*> viewer) {
	if (_list.empty() && _skippedBefore == 0 && _skippedAfter == 0) {
		viewer->slice.ids.clear();
		viewer->slice.fullCount
			= viewer->slice.skippedBefore
			= viewer->slice.skippedAfter
			= 0;
		return true;
	}
	const auto around = viewer->around;
	if (_list.empty()
		|| (!around && _skippedAfter != 0)
		|| (around > 0 && around < _list.back())
		|| (around > _list.front())) {
		loadAround(around);
		return false;
	}
	const auto i = around
		? ranges::lower_bound(_list, around, std::greater<>())
		: end(_list);
	const auto availableBefore = (i - begin(_list));
	const auto availableAfter = (end(_list) - i);
	const auto useBefore = std::min(availableBefore, viewer->limitBefore);
	const auto useAfter = std::min(availableAfter, viewer->limitAfter + 1);
	const auto slice = &viewer->slice;
	if (_skippedBefore.has_value()) {
		slice->skippedBefore
			= (*_skippedBefore + (availableBefore - useBefore));
	}
	if (_skippedAfter.has_value()) {
		slice->skippedAfter
			= (*_skippedAfter + (availableAfter - useAfter));
	}
	const auto channelId = _history->channelId();
	slice->ids.clear();
	slice->ids.reserve(useBefore + useAfter);
	for (auto j = i - useBefore, e = i + useAfter; j != e; ++j) {
		slice->ids.emplace_back(channelId, *j);
	}
	ranges::reverse(slice->ids);
	slice->fullCount = _fullCount.current();
	if (_skippedBefore != 0 && useBefore < viewer->limitBefore) {
		loadBefore();
	}
	if (_skippedAfter != 0 && useAfter < viewer->limitAfter + 1) {
		loadAfter();
	}
	return true;
}

bool RepliesList::applyUpdate(
		not_null<Viewer*> viewer,
		const MessageUpdate &update) {
	if (update.item->history() != _history
		|| update.item->replyToTop() != _rootId) {
		return false;
	}
	const auto id = update.item->id;
	const auto i = ranges::lower_bound(_list, id, std::greater<>());
	if (update.flags & MessageUpdate::Flag::Destroyed) {
		if (i == end(_list) || *i != id) {
			return false;
		}
		_list.erase(i);
		if (_skippedBefore && _skippedAfter) {
			_fullCount = *_skippedBefore + _list.size() + *_skippedAfter;
		} else if (const auto known = _fullCount.current()) {
			if (*known > 0) {
				_fullCount = (*known - 1);
			}
		}
	} else if (_skippedAfter != 0) {
		return false;
	} else {
		if (i != end(_list) && *i == id) {
			return false;
		}
		_list.insert(i, id);
		if (_skippedBefore && _skippedAfter) {
			_fullCount = *_skippedBefore + _list.size() + *_skippedAfter;
		} else if (const auto known = _fullCount.current()) {
			_fullCount = *known + 1;
		}
	}
	return true;
}

Histories &RepliesList::histories() {
	return _history->owner().histories();
}

void RepliesList::loadAround(MsgId id) {
	if (_loadingAround && *_loadingAround == id) {
		return;
	}
	histories().cancelRequest(base::take(_beforeId));
	histories().cancelRequest(base::take(_afterId));

	const auto send = [=](Fn<void()> finish) {
		return _history->session().api().request(MTPmessages_GetReplies(
			_history->peer->input,
			MTP_int(_rootId),
			MTP_int(id),
			MTP_int(id ? (-kMessagesPerPage / 2) : 0),
			MTP_int(kMessagesPerPage),
			MTP_int(0),
			MTP_int(0),
			MTP_int(0)
		)).done([=](const MTPmessages_Messages &result) {
			_beforeId = 0;
			_loadingAround = std::nullopt;
			finish();

			if (!id) {
				_skippedAfter = 0;
			} else {
				_skippedAfter = std::nullopt;
			}
			_skippedBefore = std::nullopt;
			_list.clear();
			if (processMessagesIsEmpty(result)) {
				_fullCount = _skippedBefore = _skippedAfter = 0;
			}
		}).fail([=](const RPCError &error) {
			_beforeId = 0;
			_loadingAround = std::nullopt;
			finish();
		}).send();
	};
	_loadingAround = id;
	_beforeId = histories().sendRequest(
		_history,
		Histories::RequestType::History,
		send);
}

void RepliesList::loadBefore() {
	Expects(!_list.empty());

	if (_loadingAround) {
		histories().cancelRequest(base::take(_beforeId));
	} else if (_beforeId) {
		return;
	}

	const auto last = _list.back();
	const auto send = [=](Fn<void()> finish) {
		return _history->session().api().request(MTPmessages_GetReplies(
			_history->peer->input,
			MTP_int(_rootId),
			MTP_int(last),
			MTP_int(0),
			MTP_int(kMessagesPerPage),
			MTP_int(0),
			MTP_int(0),
			MTP_int(0)
		)).done([=](const MTPmessages_Messages &result) {
			_beforeId = 0;
			finish();

			if (_list.empty()) {
				return;
			} else if (_list.back() != last) {
				loadBefore();
			} else if (processMessagesIsEmpty(result)) {
				_skippedBefore = 0;
				if (_skippedAfter == 0) {
					_fullCount = _list.size();
				}
			}
		}).fail([=](const RPCError &error) {
			_beforeId = 0;
			finish();
		}).send();
	};
	_beforeId = histories().sendRequest(
		_history,
		Histories::RequestType::History,
		send);
}

void RepliesList::loadAfter() {
	Expects(!_list.empty());

	if (_afterId) {
		return;
	}

	const auto first = _list.front();
	const auto send = [=](Fn<void()> finish) {
		return _history->session().api().request(MTPmessages_GetReplies(
			_history->peer->input,
			MTP_int(_rootId),
			MTP_int(first + 1),
			MTP_int(-kMessagesPerPage),
			MTP_int(kMessagesPerPage),
			MTP_int(0),
			MTP_int(0),
			MTP_int(0)
		)).done([=](const MTPmessages_Messages &result) {
			_afterId = 0;
			finish();

			if (_list.empty()) {
				return;
			} else if (_list.front() != first) {
				loadAfter();
			} else if (processMessagesIsEmpty(result)) {
				_skippedAfter = 0;
				if (_skippedBefore == 0) {
					_fullCount = _list.size();
				}
			}
		}).fail([=](const RPCError &error) {
			_afterId = 0;
			finish();
		}).send();
	};
	_afterId = histories().sendRequest(
		_history,
		Histories::RequestType::History,
		send);
}

bool RepliesList::processMessagesIsEmpty(const MTPmessages_Messages &result) {
	const auto guard = gsl::finally([&] { _partLoaded.fire({}); });

	_fullCount = result.match([&](const MTPDmessages_messagesNotModified &) {
		LOG(("API Error: received messages.messagesNotModified! "
			"(HistoryWidget::messagesReceived)"));
		return 0;
	}, [&](const MTPDmessages_messages &data) {
		return int(data.vmessages().v.size());
	}, [&](const MTPDmessages_messagesSlice &data) {
		return data.vcount().v;
	}, [&](const MTPDmessages_channelMessages &data) {
		if (_history->peer->isChannel()) {
			_history->peer->asChannel()->ptsReceived(data.vpts().v);
		} else {
			LOG(("API Error: received messages.channelMessages when "
				"no channel was passed! (HistoryWidget::messagesReceived)"));
		}
		return data.vcount().v;
	});

	auto &owner = _history->owner();
	const auto list = result.match([&](
			const MTPDmessages_messagesNotModified &) {
		LOG(("API Error: received messages.messagesNotModified! "
			"(HistoryWidget::messagesReceived)"));
		return QVector<MTPMessage>();
	}, [&](const auto &data) {
		owner.processUsers(data.vusers());
		owner.processChats(data.vchats());
		return data.vmessages().v;
	});
	if (list.isEmpty()) {
		return true;
	}

	const auto id = IdFromMessage(list.front());
	const auto toFront = !_list.empty() && (id > _list.front());
	const auto clientFlags = MTPDmessage_ClientFlags();
	const auto type = NewMessageType::Existing;
	auto refreshed = std::vector<MsgId>();
	if (toFront) {
		refreshed.reserve(_list.size() + list.size());
	}
	for (const auto &message : list) {
		if (const auto item = owner.addNewMessage(message, clientFlags, type)) {
			if (item->replyToTop() == _rootId) {
				if (toFront) {
					refreshed.push_back(item->id);
				} else {
					_list.push_back(item->id);
				}
			}
		}
	}
	if (toFront) {
		refreshed.insert(refreshed.end(), _list.begin(), _list.end());
		_list = std::move(refreshed);
	}
	return false;
}

} // namespace Data