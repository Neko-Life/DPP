#include <dpp/discord.h>
#include <dpp/event.h>
#include <string>
#include <iostream>
#include <fstream>
#include <dpp/discordclient.h>
#include <dpp/discord.h>
#include <dpp/cache.h>
#include <dpp/stringops.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace dpp { namespace events {

using namespace dpp;

/**
 * @brief Handle event
 * 
 * @param client Websocket client (current shard)
 * @param j JSON data for the event
 * @param raw Raw JSON string
 */
void invite_create::handle(DiscordClient* client, json &j, const std::string &raw) {
	if (client->creator->dispatch.invite_create) {
		json& d = j["d"];
		dpp::invite_create_t ci(client, raw);
		ci.created_invite = dpp::invite().fill_from_json(&d);
		client->creator->dispatch.invite_create(ci);
	}
}

}};