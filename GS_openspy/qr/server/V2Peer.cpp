#include <stdio.h>
#include <stdlib.h>
#include <sstream>
#include "QRServer.h"


#include "QRPeer.h"
#include "QRDriver.h"
#include "V2Peer.h"

#include <OS/legacy/enctypex_decoder.h>
#include <OS/legacy/helpers.h>

#define PACKET_QUERY              0x00  //S -> C
#define PACKET_CHALLENGE          0x01  //S -> C
#define PACKET_ECHO               0x02  //S -> C (purpose..?)
#define PACKET_ECHO_RESPONSE      0x05  // 0x05, not 0x03 (order) | C -> S
#define PACKET_HEARTBEAT          0x03  //C -> S
#define PACKET_ADDERROR           0x04  //S -> C
#define PACKET_CLIENT_MESSAGE     0x06  //S -> C
#define PACKET_CLIENT_MESSAGE_ACK 0x07  //C -> S
#define PACKET_KEEPALIVE          0x08  //S -> C | C -> S
#define PACKET_PREQUERY_IP_VERIFY 0x09  //S -> C
#define PACKET_AVAILABLE          0x09  //C -> S
#define PACKET_CLIENT_REGISTERED  0x0A  //S -> C

#define QR2_OPTION_USE_QUERY_CHALLENGE 128 //backend flag

#define QR_MAGIC_1 0xFE
#define QR_MAGIC_2 0xFD

namespace QR {
	V2Peer::V2Peer(Driver *driver, struct sockaddr_in *address_info, int sd) : Peer(driver,address_info,sd,2) {
		m_recv_instance_key = false;

		m_sent_challenge = false;
		m_server_pushed = false;
		memset(&m_instance_key, 0, sizeof(m_instance_key));
		memset(&m_challenge, 0, sizeof(m_challenge));

		m_server_info.m_address = m_address_info;

		gettimeofday(&m_last_ping, NULL);
		gettimeofday(&m_last_recv, NULL);

	}
	V2Peer::~V2Peer() {
	}

	void V2Peer::SendPacket(OS::Buffer &buffer) {

		m_peer_stats.packets_out++;
		m_peer_stats.bytes_out += buffer.size();

		sendto(m_sd,(const char *)buffer.GetHead(),buffer.size(),0,(struct sockaddr *)&m_address_info, sizeof(sockaddr_in));
	}
	void V2Peer::handle_packet(char *recvbuf, int len) {
		OS::Buffer buffer(recvbuf, len);

		m_peer_stats.packets_in++;
		m_peer_stats.bytes_in += len;

		uint8_t type = buffer.ReadByte();

		uint8_t instance_key[REQUEST_KEY_LEN];
		if(m_recv_instance_key || type == PACKET_AVAILABLE)
			buffer.ReadBuffer(&instance_key, REQUEST_KEY_LEN);

		if(m_recv_instance_key) {
			if(memcmp((uint8_t *)&instance_key, (uint8_t *)&m_instance_key, sizeof(instance_key)) != 0) {
				OS::LogText(OS::ELogLevel_Info, "[%s] Instance key mismatch/possible spoofed packet", OS::Address(m_address_info).ToString().c_str());
				return;
			}
		}


		gettimeofday(&m_last_recv, NULL);

		switch(type) {
			case PACKET_AVAILABLE:
				handle_available(buffer);
				break;
			case PACKET_HEARTBEAT:
				handle_heartbeat(buffer);
			break;
			case PACKET_CHALLENGE:
				handle_challenge(buffer);
			break;
			case PACKET_KEEPALIVE:
				handle_keepalive(buffer);
			break;
			case PACKET_CLIENT_MESSAGE_ACK:
				OS::LogText(OS::ELogLevel_Info, "[%s] Client Message ACK", OS::Address(m_address_info).ToString().c_str());
			break;
		}
	}
	void V2Peer::handle_challenge(OS::Buffer &buffer) {
		char challenge_resp[90] = { 0 };
		int outlen = 0;
		uint8_t *p = (uint8_t *)challenge_resp;
		if(m_server_info.m_game.secretkey[0] == 0) {
			send_error(true, "Unknown game");
			return;
		}
		gsseckey((unsigned char *)&challenge_resp, (unsigned char *)&m_challenge, (unsigned char *)&m_server_info.m_game.secretkey, 0);
		if(strcmp(buffer.ReadNTS().c_str(),challenge_resp) == 0) { //matching challenge
			OS::LogText(OS::ELogLevel_Info, "[%s] Server pushed, gamename: %s", OS::Address(m_address_info).ToString().c_str(), m_server_info.m_game.gamename);
			if(m_sent_challenge) {
				MM::MMPushRequest req;
				req.peer = this;
				req.server = m_dirty_server_info;
				m_server_info = m_dirty_server_info;
				req.peer->IncRef();
				req.type = MM::EMMPushRequestType_PushServer;
				m_server_pushed = true;
				m_peer_stats.pending_requests++;
				MM::m_task_pool->AddRequest(req);
			}
			m_sent_challenge = true;
		}
		else {
			OS::LogText(OS::ELogLevel_Info, "[%s] Incorrect challenge for gamename: %s", OS::Address(m_address_info).ToString().c_str(), m_server_info.m_game.gamename);
		}
	}
	void V2Peer::handle_keepalive(OS::Buffer &buffer) {
		uint32_t key = buffer.ReadInt();
		if (key == *(uint32_t *)&m_instance_key) {
			buffer.WriteByte(QR_MAGIC_1);
			buffer.WriteByte(QR_MAGIC_2);
			buffer.WriteInt(key);
			SendPacket(buffer);
		}
	}
	void V2Peer::handle_heartbeat(OS::Buffer &buffer) {
		unsigned int i = 0;

		MM::ServerInfo server_info, old_server_info = m_server_info;
		server_info.m_game = m_server_info.m_game;
		server_info.m_address = m_server_info.m_address;
		server_info.id = m_server_info.id;
		server_info.groupid = m_server_info.groupid;

		std::string key, value;

		if(!m_recv_instance_key) {
			buffer.ReadBuffer(&m_instance_key, sizeof(m_instance_key));
			m_recv_instance_key = true;
		}

		std::stringstream ss;

		while(true) {

			if(i%2 == 0) {
				key = buffer.ReadNTS();
				if (key.length() == 0) break;
			} else {
				value = buffer.ReadNTS();
				ss << "(" << key << "," << value << ") ";
			}

			if(value.length() > 0) {
				server_info.m_keys[key] = value;
				value = std::string();
			}
			i++;
		}

		OS::LogText(OS::ELogLevel_Info, "[%s] HB Keys: %s", OS::Address(m_address_info).ToString().c_str(), ss.str().c_str());
		ss.str("");


		uint16_t num_values = 0;

		while((num_values = htons(buffer.ReadShort()))) {
			std::vector<std::string> nameValueList;
			if(buffer.remaining() <= 3) {
				break;
			}
			uint32_t num_keys = 0;
			std::string x;
			while(buffer.remaining() && (x = buffer.ReadNTS()).length() > 0) {
				nameValueList.push_back(x);
				num_keys++;
			}
			unsigned int player=0,num_keys_t = num_keys,num_values_t = num_values*num_keys;
			i = 0;

			while(num_values_t--) {
				std::string name = nameValueList.at(i);

				x = buffer.ReadNTS();

				if(isTeamString(name.c_str())) {
					if(server_info.m_team_keys[name].size() <= player) {
						server_info.m_team_keys[name].push_back(x);
					}
					else {
						server_info.m_team_keys[name][player] = x;
					}
					ss << "T(" << player << ") (" << name.c_str() << "," << x << ") ";
				} else {
					if(server_info.m_player_keys[name].size() <= player) {
						server_info.m_player_keys[name].push_back(x);
					} else {
						server_info.m_player_keys[name][player] = x;
					}
					ss << "P(" << player << ") (" << name.c_str() << "," << x << " ) ";
				}
				i++;
				if(i >= num_keys) {
					player++;
					i = 0;
				}
			}
		}


		OS::LogText(OS::ELogLevel_Info, "[%s] HB Keys: %s", OS::Address(m_address_info).ToString().c_str(), ss.str().c_str());
		ss.str("");

		m_dirty_server_info = server_info;

		//register gamename
		MM::MMPushRequest req;
		req.peer = this;
		if (server_info.m_game.gameid != 0) {
			if (m_server_pushed) {
				if (server_info.m_keys.find("statechanged") != server_info.m_keys.end() && atoi(server_info.m_keys["statechanged"].c_str()) == 2) {
					Delete();
					return;
				}
				struct timeval current_time;
				gettimeofday(&current_time, NULL);
				if (current_time.tv_sec - m_last_heartbeat.tv_sec > HB_THROTTLE_TIME) {
					m_server_info_dirty = false;
					m_server_info = server_info;
					gettimeofday(&m_last_heartbeat, NULL);
					req.server = m_server_info;
					req.old_server = old_server_info;
					req.peer->IncRef();
					req.type = MM::EMMPushRequestType_UpdateServer;
					m_peer_stats.pending_requests++;
					MM::m_task_pool->AddRequest(req);
				} else {
					m_server_info_dirty = true;
				}
			}
			else {
				OnGetGameInfo(server_info.m_game, (void *)1);
			}
		}
		else if(!m_sent_game_query){
			m_sent_game_query = true;
			req.peer->IncRef();
			req.extra = (void *)1;
			req.gamename = server_info.m_keys["gamename"];
			req.type = MM::EMMPushRequestType_GetGameInfoByGameName;
			m_peer_stats.pending_requests++;
			MM::m_task_pool->AddRequest(req);
		}
	}
	void V2Peer::handle_available(OS::Buffer &buffer) {
		MM::MMPushRequest req;
		req.peer = this;
		req.peer->IncRef();
		req.extra = (void *)2;

		req.gamename = buffer.ReadNTS();

		OS::LogText(OS::ELogLevel_Info, "[%s] Got available request: %s", OS::Address(m_address_info).ToString().c_str(), req.gamename.c_str());
		req.type = MM::EMMPushRequestType_GetGameInfoByGameName;
		m_peer_stats.pending_requests++;
		MM::m_task_pool->AddRequest(req);
	}
	void V2Peer::OnGetGameInfo(OS::GameData game_info, void *extra) {
		m_peer_stats.from_game = game_info;
		if (extra == (void *)1) {
			m_server_info.m_game = game_info;
			m_dirty_server_info.m_game = game_info;
			if (m_server_info.m_game.secretkey[0] == 0) {
				send_error(true, "Game not found");
				return;
			}

			if (m_server_info.m_keys.find("statechanged") != m_server_info.m_keys.end() && atoi(m_server_info.m_keys["statechanged"].c_str()) == 2) {
				Delete();
				return;
			}

			if (!m_sent_challenge) {
				send_challenge();
			}

			//TODO: check if changed and only push changes
			if (m_server_pushed) {
				struct timeval current_time;
				gettimeofday(&current_time, NULL);
				if (current_time.tv_sec - m_last_heartbeat.tv_sec > HB_THROTTLE_TIME) {
					m_server_info_dirty = false;
					gettimeofday(&m_last_heartbeat, NULL);
					MM::MMPushRequest req;
					req.peer = this;
					m_dirty_server_info = m_server_info;
					req.server = m_server_info;
					req.peer->IncRef();
					req.type = MM::EMMPushRequestType_UpdateServer_NoDiff;
					m_peer_stats.pending_requests++;
					MM::m_task_pool->AddRequest(req);
				}
			}
		}
		else if(extra == (void *)2) {
			if (game_info.secretkey[0] == 0) {
				game_info.disabled_services = OS::QR2_GAME_AVAILABLE;
			}
			OS::Buffer buffer;
			buffer.WriteByte(QR_MAGIC_1);
			buffer.WriteByte(QR_MAGIC_2);

			buffer.WriteByte(PACKET_AVAILABLE);
			buffer.WriteInt(htonl(game_info.disabled_services));
			SendPacket(buffer);
			Delete();
		}
	}
	void V2Peer::send_error(bool die, const char *fmt, ...) {

		//XXX: make all these support const vars
		OS::Buffer buffer;
		buffer.WriteNTS(fmt);
		SendPacket(buffer);
		if(die) {
			m_timeout_flag = false;
			Delete();
		}

		OS::LogText(OS::ELogLevel_Info, "[%s] Error:", OS::Address(m_address_info).ToString().c_str(), fmt);
	}
	void V2Peer::send_ping() {
		//check for timeout
		struct timeval current_time;


		gettimeofday(&current_time, NULL);
		if(current_time.tv_sec - m_last_ping.tv_sec > QR2_PING_TIME) {
			OS::Buffer buffer;

			gettimeofday(&m_last_ping, NULL);

			buffer.WriteByte(QR_MAGIC_1);
			buffer.WriteByte(QR_MAGIC_2);

			buffer.WriteByte(PACKET_KEEPALIVE);
			buffer.WriteBuffer((uint8_t *)&m_instance_key, sizeof(m_instance_key));
			SendPacket(buffer);
		}

	}
	void V2Peer::think(bool listener_waiting) {
		send_ping();
		SubmitDirtyServer();

		//check for timeout
		struct timeval current_time;
		gettimeofday(&current_time, NULL);
		if(current_time.tv_sec - m_last_recv.tv_sec > QR2_PING_TIME) {
			m_timeout_flag = true;
			send_error(true, "Timeout");
			
		}
	}

	void V2Peer::send_challenge() {
		OS::Buffer buffer;

		memset(&m_challenge, 0, sizeof(m_challenge));
		gen_random((char *)&m_challenge,sizeof(m_challenge)-1);

		uint16_t *backend_flags = (uint16_t *)&m_challenge[13];
		//*backend_flags &= ~QR2_OPTION_USE_QUERY_CHALLENGE;


		buffer.WriteByte(QR_MAGIC_1);
		buffer.WriteByte(QR_MAGIC_2);

		buffer.WriteByte(PACKET_CHALLENGE);
		buffer.WriteBuffer((void *)&m_instance_key, sizeof(m_instance_key));
		buffer.WriteNTS(m_challenge);

		SendPacket(buffer);

		m_sent_challenge = true;
	}
	void V2Peer::SendClientMessage(uint8_t *data, int data_len) {
		OS::Buffer buffer;
		uint32_t key = rand() % 100000 + 1;

		buffer.WriteByte(QR_MAGIC_1);
		buffer.WriteByte(QR_MAGIC_2);

		buffer.WriteByte(PACKET_CLIENT_MESSAGE);
		buffer.WriteBuffer((void *)&m_instance_key, sizeof(m_instance_key));

		buffer.WriteInt(key);
		buffer.WriteBuffer(data, data_len);
		SendPacket(buffer);
	}
	void V2Peer::OnRegisteredServer(int pk_id, void *extra) {
		OS::Buffer buffer;
		m_server_info.id = pk_id;

		buffer.WriteByte(QR_MAGIC_1);
		buffer.WriteByte(QR_MAGIC_2);

		buffer.WriteByte(PACKET_CLIENT_REGISTERED);
		buffer.WriteBuffer((void *)&m_instance_key, sizeof(m_instance_key));
		SendPacket(buffer);
		
	}
}
