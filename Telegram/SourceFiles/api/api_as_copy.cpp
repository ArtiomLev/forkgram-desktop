/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "api/api_as_copy.h"

#include "apiwrap.h"
#include "api/api_sending.h"
#include "api/api_text_entities.h"
#include "base/random.h"
#include "base/unixtime.h"
#include "data/data_document.h"
#include "data/data_drafts.h"
#include "data/data_histories.h"
#include "data/data_photo.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "chat_helpers/message_field.h"

namespace Api::AsCopy {
namespace {

[[nodiscard]] TimeId ScheduledDraft() {
	return base::unixtime::now() + 3600 * 24 * 350;
}

MTPInputSingleMedia PrepareAlbumItemMedia(
		not_null<HistoryItem*> item,
		const MTPInputMedia &media,
		uint64 randomId,
		bool emptyText,
		TextWithTags comment) {
	auto commentEntities = TextWithEntities {
		comment.text,
		TextUtilities::ConvertTextTagsToEntities(comment.tags)
	};

	auto caption = item->originalText();
	TextUtilities::Trim(caption);
	auto sentEntities = Api::EntitiesToMTP(
		&item->history()->session(),
		emptyText ? commentEntities.entities : caption.entities,
		Api::ConvertOption::SkipLocal);
	const auto flags = !sentEntities.v.isEmpty()
		? MTPDinputSingleMedia::Flag::f_entities
		: MTPDinputSingleMedia::Flag(0);

	return MTP_inputSingleMedia(
		MTP_flags(flags),
		media,
		MTP_long(randomId),
		MTP_string(emptyText ? commentEntities.text : caption.text),
		sentEntities);
}

MTPinputMedia InputMediaFromItem(not_null<HistoryItem*> i) {
	if (const auto document = i->media()->document()) {
		return MTP_inputMediaDocument(
			MTP_flags(MTPDinputMediaDocument::Flag(0)),
			document->mtpInput(),
			MTP_int(0),
			MTPstring());
	} else if (const auto photo = i->media()->photo()) {
		return MTP_inputMediaPhoto(
			MTP_flags(MTPDinputMediaPhoto::Flag(0)),
			photo->mtpInput(),
			MTP_int(0));
	} else {
		return MTP_inputMediaEmpty();
	}
}

FullReplyTo ReplyToIdFromDraft(not_null<PeerData*> peer) {
	const auto history = peer->owner().history(peer);
	const auto replyTo = [&]() -> FullReplyTo {
		if (const auto localDraft = history->localDraft(0)) {
			return localDraft->reply;
		} else if (const auto cloudDraft = history->cloudDraft(0)) {
			return cloudDraft->reply;
		} else {
			return {};
		}
	}();
	if (replyTo) {
		history->clearCloudDraft(0);
		history->clearLocalDraft(0);
		peer->session().api().request(
			MTPmessages_SaveDraft(
				MTP_flags(MTPmessages_SaveDraft::Flags(0)),
				MTP_inputReplyToStory(MTP_inputPeerEmpty(), MTPint()),
				history->peer->input,
				MTPstring(),
				MTPVector<MTPMessageEntity>(),
				MTP_inputMediaEmpty()
		)).send();
	}
	return replyTo;
}

} // namespace

void SendAlbumFromItems(
		HistoryItemsList items,
		ToSend &&toSend,
		bool andDelete) {
	if (items.empty()) {
		return;
	}
	const auto history = items.front()->history();
	const auto ids = history->owner().itemsToIds(items);
	auto medias = QVector<MTPInputSingleMedia>();
	for (const auto &i : items) {
		medias.push_back(PrepareAlbumItemMedia(
			i,
			InputMediaFromItem(i),
			base::RandomValue<uint64>(),
			toSend.emptyText,
			medias.empty() ? toSend.comment : TextWithTags()));
	}
	auto &api = history->owner().session().api();

	for (const auto &peer : toSend.peers) {
		const auto replyTo = ReplyToIdFromDraft(peer);

		const auto flags = MTPmessages_SendMultiMedia::Flags(0)
			| (replyTo
				? MTPmessages_SendMultiMedia::Flag::f_reply_to
				: MTPmessages_SendMultiMedia::Flag(0))
			| (toSend.silent
				? MTPmessages_SendMultiMedia::Flag::f_silent
				: MTPmessages_SendMultiMedia::Flag(0))
			| (toSend.scheduledDraft
				? MTPmessages_SendMultiMedia::Flag::f_schedule_date
				: MTPmessages_SendMultiMedia::Flag(0));
		api.request(MTPmessages_SendMultiMedia(
			MTP_flags(flags),
			peer->input,
			ReplyToForMTP(history, replyTo),
			MTP_vector<MTPInputSingleMedia>(medias),
			MTP_int(toSend.scheduledDraft ? ScheduledDraft() : 0),
			MTP_inputPeerEmpty(),
			MTPInputQuickReplyShortcut()
		)).done([=](const MTPUpdates &result) {
			history->owner().session().api().applyUpdates(result);

			if (andDelete) {
				history->owner().histories().deleteMessages(ids, true);
				history->owner().sendHistoryChangeNotifications();
			}
		}).fail([=](const MTP::Error &error) {
		}).send();
	}
}

void SendExistingAlbumFromItem(
		not_null<HistoryItem*> item,
		Api::AsCopy::ToSend &&toSend) {
	if (!item->groupId()) {
		return;
	}
	SendAlbumFromItems(
		item->history()->owner().groups().find(item)->items,
		std::move(toSend),
		false);
}

void SendExistingMediaFromItem(
		not_null<HistoryItem*> item,
		Api::AsCopy::ToSend &&toSend) {
	for (const auto peer : toSend.peers) {
		const auto history = peer->owner().history(peer);
		auto message = MessageToSend(SendAction{ history });
		if (!item->media()) {
			message.textWithTags = PrepareEditText(item);
			history->session().api().sendMessage(std::move(message));
			continue;
		}
		message.textWithTags = toSend.emptyText
			? toSend.comment
			: PrepareEditText(item);
		message.action.options.silent = toSend.silent;
		if (toSend.scheduledDraft) {
			message.action.options.scheduled = ScheduledDraft();
		}
		message.action.replyTo = ReplyToIdFromDraft(peer);
		if (const auto document = item->media()->document()) {
			Api::SendExistingDocument(
				std::move(message),
				document,
				item->fullId());
		} else if (const auto photo = item->media()->photo()) {
			Api::SendExistingPhoto(std::move(message), photo, item->fullId());
		}
	}
}

} // namespace Api::AsCopy
