// climode.cpp
//
// Copyright (c) 2024-2026 Kristofer Berggren
// All rights reserved.
//
// nchat is distributed under the MIT license, see LICENSE for details.

#include "climode.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <sstream>
#include <unordered_set>

#include <signal.h>
#include <unistd.h>

#include "fileutil.h"
#include "log.h"
#include "messagecache.h"
#include "timeutil.h"

static volatile sig_atomic_t s_Running = 1;

static void SignalHandler(int /*p_Signal*/)
{
  s_Running = 0;
}

CliMode::CliMode()
{
}

CliMode::~CliMode()
{
}

void CliMode::SetProfile(const std::string& p_ProfileId)
{
  m_ProfileFilter = p_ProfileId;
}

void CliMode::SetOutputJson(bool p_Json)
{
  m_OutputJson = p_Json;
}

void CliMode::SetTimeout(int p_Seconds)
{
  m_TimeoutSec = p_Seconds;
}

void CliMode::AddProtocol(std::shared_ptr<Protocol> p_Protocol)
{
  m_Protocols[p_Protocol->GetProfileId()] = p_Protocol;
}

std::unordered_map<std::string, std::shared_ptr<Protocol>> CliMode::GetProtocols()
{
  return m_Protocols;
}

void CliMode::MessageHandler(std::shared_ptr<ServiceMessage> p_ServiceMessage)
{
  if (!p_ServiceMessage) return;

  std::lock_guard<std::mutex> lock(m_Mutex);

  switch (p_ServiceMessage->GetMessageType())
  {
    case ConnectNotifyType:
      {
        std::shared_ptr<ConnectNotify> notify =
          std::dynamic_pointer_cast<ConnectNotify>(p_ServiceMessage);
        if (notify)
        {
          m_Connected[notify->profileId] = notify->success;
          LOG_DEBUG("cli connect notify %s success=%d", notify->profileId.c_str(), notify->success);

          // Check if all protocols are connected
          bool allConnected = true;
          for (auto& protocol : m_Protocols)
          {
            if (m_Connected.find(protocol.first) == m_Connected.end())
            {
              allConnected = false;
              break;
            }
          }
          m_AllConnected = allConnected;
          m_CondVar.notify_all();
        }
      }
      break;

    case NewChatsNotifyType:
      {
        std::shared_ptr<NewChatsNotify> notify =
          std::dynamic_pointer_cast<NewChatsNotify>(p_ServiceMessage);
        if (notify && notify->success)
        {
          for (auto& chatInfo : notify->chatInfos)
          {
            m_ChatInfos.push_back(std::make_pair(notify->profileId, chatInfo));
          }
          m_ResponseReceived = true;
          m_LastSuccess = true;
          m_CondVar.notify_all();
        }
      }
      break;

    case NewContactsNotifyType:
      {
        std::shared_ptr<NewContactsNotify> notify =
          std::dynamic_pointer_cast<NewContactsNotify>(p_ServiceMessage);
        if (notify)
        {
          for (auto& contact : notify->contactInfos)
          {
            m_ContactInfos[notify->profileId][contact.id] = contact;
          }
          m_ResponseReceived = true;
          m_LastSuccess = true;
          m_CondVar.notify_all();
        }
      }
      break;

    case NewMessagesNotifyType:
      {
        std::shared_ptr<NewMessagesNotify> notify =
          std::dynamic_pointer_cast<NewMessagesNotify>(p_ServiceMessage);
        if (notify)
        {
          if (m_WatchMode)
          {
            // In watch mode, output messages immediately
            for (auto& msg : notify->chatMessages)
            {
              if (!m_WatchChatFilter.empty() && notify->chatId != m_WatchChatFilter)
              {
                continue;
              }

              if (m_OutputJson)
              {
                std::cout << ChatMessageToJson(msg, notify->profileId, notify->chatId) << "\n";
              }
              else
              {
                std::string senderName = msg.isOutgoing
                  ? "You"
                  : GetContactName(notify->profileId, msg.senderId);
                std::string timeStr = TimeUtil::GetTimeString(msg.timeSent, false /*p_IsExport*/);
                std::cout << "[" << timeStr << "] "
                          << notify->chatId << " | "
                          << senderName << ": " << msg.text << "\n";
              }
              std::cout.flush();
            }
          }
          else
          {
            for (auto& msg : notify->chatMessages)
            {
              m_ChatMessages.push_back(msg);
            }
            m_ChatMessagesProfileId = notify->profileId;
            m_ChatMessagesChatId = notify->chatId;
            m_ResponseReceived = true;
            m_LastSuccess = notify->success;
            m_CondVar.notify_all();
          }
        }
      }
      break;

    case SendMessageNotifyType:
      {
        std::shared_ptr<SendMessageNotify> notify =
          std::dynamic_pointer_cast<SendMessageNotify>(p_ServiceMessage);
        if (notify)
        {
          m_SendDone = true;
          m_SendSuccess = notify->success;
          m_CondVar.notify_all();
        }
      }
      break;

    case MarkMessageReadNotifyType:
      {
        std::shared_ptr<MarkMessageReadNotify> notify =
          std::dynamic_pointer_cast<MarkMessageReadNotify>(p_ServiceMessage);
        if (notify)
        {
          m_MarkReadDone = true;
          m_MarkReadSuccess = notify->success;
          m_CondVar.notify_all();
        }
      }
      break;

    case FindMessageNotifyType:
      {
        std::shared_ptr<FindMessageNotify> notify =
          std::dynamic_pointer_cast<FindMessageNotify>(p_ServiceMessage);
        if (notify)
        {
          m_FindDone = true;
          m_FindSuccess = notify->success;
          m_FindMsgId = notify->msgId;
          m_CondVar.notify_all();
        }
      }
      break;

    default:
      break;
  }
}

bool CliMode::WaitForConnection(int p_TimeoutSec)
{
  std::unique_lock<std::mutex> lock(m_Mutex);
  return m_CondVar.wait_for(lock, std::chrono::seconds(p_TimeoutSec), [this]
  {
    return m_AllConnected;
  });
}

std::string CliMode::ResolveProfile()
{
  if (!m_ProfileFilter.empty())
  {
    if (m_Protocols.find(m_ProfileFilter) == m_Protocols.end())
    {
      std::cerr << "error: unknown profile '" << m_ProfileFilter << "'\n";
      std::cerr << "available profiles:\n";
      for (auto& p : m_Protocols)
      {
        std::cerr << "  " << p.first << "\n";
      }
      return "";
    }
    return m_ProfileFilter;
  }

  if (m_Protocols.size() == 1)
  {
    return m_Protocols.begin()->first;
  }

  // Multiple profiles, no filter
  return ""; // empty means all profiles
}

int CliMode::Run(const std::string& p_Command, const std::map<std::string, std::string>& p_Options)
{
  if (p_Command == "list-profiles")
  {
    return CmdListProfiles();
  }

  // For all other commands, we need a connection
  if (!WaitForConnection(m_TimeoutSec))
  {
    std::cerr << "error: connection timed out after " << m_TimeoutSec << " seconds\n";
    return 1;
  }

  // Fetch contacts for name resolution (for all connected profiles)
  for (auto& protocol : m_Protocols)
  {
    MessageCache::FetchContacts(protocol.first);
  }

  // Get options
  auto getOpt = [&](const std::string& key) -> std::string
  {
    auto it = p_Options.find(key);
    return (it != p_Options.end()) ? it->second : "";
  };

  int limit = 0;
  std::string limitStr = getOpt("limit");
  if (!limitStr.empty())
  {
    limit = std::stoi(limitStr);
  }

  std::string chatId = getOpt("chat");
  std::string text = getOpt("text");
  std::string filePath = getOpt("file");
  std::string query = getOpt("query");
  std::string msgId = getOpt("msg");

  if (p_Command == "list-chats")
  {
    return CmdListChats(limit);
  }
  else if (p_Command == "list-contacts")
  {
    return CmdListContacts();
  }
  else if (p_Command == "read")
  {
    if (chatId.empty())
    {
      std::cerr << "error: --chat is required for 'read' command\n";
      return 1;
    }
    if (limit <= 0) limit = 20;
    return CmdReadMessages(chatId, limit);
  }
  else if (p_Command == "send")
  {
    if (chatId.empty())
    {
      std::cerr << "error: --chat is required for 'send' command\n";
      return 1;
    }
    // If no --text, read from stdin
    if (text.empty())
    {
      std::ostringstream oss;
      oss << std::cin.rdbuf();
      text = oss.str();
      // Trim trailing newline
      while (!text.empty() && text.back() == '\n')
      {
        text.pop_back();
      }
    }
    if (text.empty() && filePath.empty())
    {
      std::cerr << "error: --text or stdin input or --file is required for 'send' command\n";
      return 1;
    }
    return CmdSendMessage(chatId, text, filePath);
  }
  else if (p_Command == "search")
  {
    if (chatId.empty())
    {
      std::cerr << "error: --chat is required for 'search' command\n";
      return 1;
    }
    if (query.empty())
    {
      std::cerr << "error: --query is required for 'search' command\n";
      return 1;
    }
    return CmdSearch(chatId, query);
  }
  else if (p_Command == "mark-read")
  {
    if (chatId.empty())
    {
      std::cerr << "error: --chat is required for 'mark-read' command\n";
      return 1;
    }
    if (msgId.empty())
    {
      std::cerr << "error: --msg is required for 'mark-read' command\n";
      return 1;
    }
    return CmdMarkRead(chatId, msgId);
  }
  else if (p_Command == "watch")
  {
    return CmdWatch(chatId);
  }
  else
  {
    std::cerr << "error: unknown command '" << p_Command << "'\n";
    return 1;
  }
}

int CliMode::CmdListProfiles()
{
  if (m_OutputJson)
  {
    std::string json = "{\"profiles\":[";
    bool first = true;
    for (auto& protocol : m_Protocols)
    {
      if (!first) json += ",";
      first = false;
      json += "{\"id\":\"" + EscapeJson(protocol.first) + "\"";
      json += ",\"name\":\"" + EscapeJson(protocol.second->GetProfileDisplayName()) + "\"";
      json += ",\"self_id\":\"" + EscapeJson(protocol.second->GetSelfId()) + "\"";
      json += "}";
    }
    json += "]}";
    OutputJson(json);
  }
  else
  {
    OutputText("=== Profiles ===");
    int idx = 1;
    for (auto& protocol : m_Protocols)
    {
      std::string line = "[" + std::to_string(idx++) + "] " + protocol.first;
      std::string displayName = protocol.second->GetProfileDisplayName();
      if (!displayName.empty())
      {
        line += " (" + displayName + ")";
      }
      OutputText(line);
    }
  }
  return 0;
}

int CliMode::CmdListChats(int p_Limit)
{
  std::string profileId = ResolveProfile();
  if (!m_ProfileFilter.empty() && profileId.empty()) return 1;

  {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_ChatInfos.clear();
    m_ResponseReceived = false;
  }

  // Fetch chats from cache
  std::unordered_set<std::string> emptyFilter;
  if (profileId.empty())
  {
    for (auto& protocol : m_Protocols)
    {
      MessageCache::FetchChats(protocol.first, emptyFilter);
    }
  }
  else
  {
    MessageCache::FetchChats(profileId, emptyFilter);
  }

  // Sort by last message time (descending)
  std::sort(m_ChatInfos.begin(), m_ChatInfos.end(),
    [](const std::pair<std::string, ChatInfo>& a, const std::pair<std::string, ChatInfo>& b)
    {
      return a.second.lastMessageTime > b.second.lastMessageTime;
    });

  // Apply limit
  if (p_Limit > 0 && static_cast<int>(m_ChatInfos.size()) > p_Limit)
  {
    m_ChatInfos.resize(p_Limit);
  }

  if (m_OutputJson)
  {
    std::string json = "{\"chats\":[";
    bool first = true;
    for (auto& pair : m_ChatInfos)
    {
      if (!first) json += ",";
      first = false;
      json += ChatInfoToJson(pair.second, pair.first);
    }
    json += "]}";
    OutputJson(json);
  }
  else
  {
    OutputText("=== Chats ===");
    int idx = 1;
    for (auto& pair : m_ChatInfos)
    {
      const ChatInfo& info = pair.second;
      const std::string& profId = pair.first;
      std::string name = GetContactName(profId, info.id);

      std::string line = "[" + std::to_string(idx++) + "] " + name;
      line += " (" + profId + ":" + info.id + ")";
      if (info.isUnread) line += " - unread";
      if (info.isPinned) line += " - pinned";
      if (info.isMuted) line += " - muted";
      OutputText(line);
    }
  }
  return 0;
}

int CliMode::CmdListContacts()
{
  std::string profileId = ResolveProfile();
  if (!m_ProfileFilter.empty() && profileId.empty()) return 1;

  if (m_OutputJson)
  {
    std::string json = "{\"contacts\":[";
    bool first = true;
    if (profileId.empty())
    {
      for (auto& profPair : m_ContactInfos)
      {
        for (auto& contactPair : profPair.second)
        {
          if (!first) json += ",";
          first = false;
          json += ContactInfoToJson(contactPair.second, profPair.first);
        }
      }
    }
    else
    {
      auto it = m_ContactInfos.find(profileId);
      if (it != m_ContactInfos.end())
      {
        for (auto& contactPair : it->second)
        {
          if (!first) json += ",";
          first = false;
          json += ContactInfoToJson(contactPair.second, profileId);
        }
      }
    }
    json += "]}";
    OutputJson(json);
  }
  else
  {
    OutputText("=== Contacts ===");
    int idx = 1;
    auto printContacts = [&](const std::string& profId,
      const std::unordered_map<std::string, ContactInfo>& contacts)
    {
      for (auto& contactPair : contacts)
      {
        const ContactInfo& c = contactPair.second;
        std::string line = "[" + std::to_string(idx++) + "] ";
        line += c.name.empty() ? c.id : c.name;
        line += " (" + profId + ":" + c.id + ")";
        if (!c.phone.empty()) line += " phone:" + c.phone;
        if (c.isSelf) line += " [self]";
        OutputText(line);
      }
    };

    if (profileId.empty())
    {
      for (auto& profPair : m_ContactInfos)
      {
        printContacts(profPair.first, profPair.second);
      }
    }
    else
    {
      auto it = m_ContactInfos.find(profileId);
      if (it != m_ContactInfos.end())
      {
        printContacts(profileId, it->second);
      }
    }
  }
  return 0;
}

int CliMode::CmdReadMessages(const std::string& p_ChatId, int p_Limit)
{
  std::string profileId = ResolveProfile();
  if (!m_ProfileFilter.empty() && profileId.empty()) return 1;

  if (profileId.empty())
  {
    std::cerr << "error: --profile is required when multiple profiles are configured\n";
    return 1;
  }

  {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_ChatMessages.clear();
    m_ResponseReceived = false;
  }

  // Fetch from cache (synchronous - calls message handler directly)
  MessageCache::FetchMessagesFrom(profileId, p_ChatId, "" /*fromMsgId*/, p_Limit, true /*sync*/);

  // Messages come in reverse chronological; reverse for display
  std::reverse(m_ChatMessages.begin(), m_ChatMessages.end());

  if (m_OutputJson)
  {
    std::string json = "{\"messages\":[";
    bool first = true;
    for (auto& msg : m_ChatMessages)
    {
      if (!first) json += ",";
      first = false;
      json += ChatMessageToJson(msg, profileId, p_ChatId);
    }
    json += "]}";
    OutputJson(json);
  }
  else
  {
    std::string chatName = GetContactName(profileId, p_ChatId);
    OutputText("=== Messages in " + chatName + " ===");
    for (auto& msg : m_ChatMessages)
    {
      std::string senderName = msg.isOutgoing
        ? "You"
        : GetContactName(profileId, msg.senderId);
      std::string timeStr = TimeUtil::GetTimeString(msg.timeSent, false /*p_IsExport*/);
      std::string line = "[" + timeStr + "] " + senderName + ": " + msg.text;
      if (!msg.fileInfo.empty())
      {
        line += " [attachment]";
      }
      if (!msg.quotedId.empty())
      {
        line += " (reply to: " + msg.quotedText + ")";
      }
      OutputText(line);
    }
  }
  return 0;
}

int CliMode::CmdSendMessage(const std::string& p_ChatId, const std::string& p_Text,
                            const std::string& p_FilePath)
{
  std::string profileId = ResolveProfile();
  if (!m_ProfileFilter.empty() && profileId.empty()) return 1;

  if (profileId.empty())
  {
    std::cerr << "error: --profile is required when multiple profiles are configured\n";
    return 1;
  }

  auto it = m_Protocols.find(profileId);
  if (it == m_Protocols.end())
  {
    std::cerr << "error: protocol not found for profile '" << profileId << "'\n";
    return 1;
  }

  {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_SendDone = false;
    m_SendSuccess = false;
  }

  std::shared_ptr<SendMessageRequest> request = std::make_shared<SendMessageRequest>();
  request->chatId = p_ChatId;
  request->chatMessage.text = p_Text;
  request->chatMessage.isOutgoing = true;

  if (!p_FilePath.empty())
  {
    request->chatMessage.fileInfo = p_FilePath;
  }

  it->second->SendRequest(request);

  // Wait for send notification
  {
    std::unique_lock<std::mutex> lock(m_Mutex);
    bool gotResponse = m_CondVar.wait_for(lock, std::chrono::seconds(m_TimeoutSec), [this]
    {
      return m_SendDone;
    });

    if (!gotResponse)
    {
      std::cerr << "error: send timed out\n";
      return 1;
    }

    if (!m_SendSuccess)
    {
      std::cerr << "error: send failed\n";
      return 1;
    }
  }

  if (m_OutputJson)
  {
    OutputJson("{\"success\":true}");
  }
  else
  {
    OutputText("Message sent.");
  }
  return 0;
}

int CliMode::CmdSearch(const std::string& p_ChatId, const std::string& p_Query)
{
  std::string profileId = ResolveProfile();
  if (!m_ProfileFilter.empty() && profileId.empty()) return 1;

  if (profileId.empty())
  {
    std::cerr << "error: --profile is required when multiple profiles are configured\n";
    return 1;
  }

  {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_FindDone = false;
    m_FindSuccess = false;
    m_FindMsgId.clear();
  }

  // Use MessageCache::FindMessage which searches the cached messages
  MessageCache::FindMessage(profileId, p_ChatId, "" /*fromMsgId*/, "" /*lastMsgId*/,
                            p_Query, "" /*findMsgId*/);

  // FindMessage is async via the queue, wait for result
  {
    std::unique_lock<std::mutex> lock(m_Mutex);
    bool gotResponse = m_CondVar.wait_for(lock, std::chrono::seconds(m_TimeoutSec), [this]
    {
      return m_FindDone;
    });

    if (!gotResponse)
    {
      std::cerr << "error: search timed out\n";
      return 1;
    }
  }

  if (m_FindSuccess && !m_FindMsgId.empty())
  {
    // Fetch the found message and surrounding context
    {
      std::lock_guard<std::mutex> lock(m_Mutex);
      m_ChatMessages.clear();
      m_ResponseReceived = false;
    }

    MessageCache::FetchMessagesFrom(profileId, p_ChatId, m_FindMsgId, 10, true /*sync*/);

    std::reverse(m_ChatMessages.begin(), m_ChatMessages.end());

    if (m_OutputJson)
    {
      std::string json = "{\"found\":true,\"msg_id\":\"" + EscapeJson(m_FindMsgId) + "\",\"messages\":[";
      bool first = true;
      for (auto& msg : m_ChatMessages)
      {
        if (!first) json += ",";
        first = false;
        json += ChatMessageToJson(msg, profileId, p_ChatId);
      }
      json += "]}";
      OutputJson(json);
    }
    else
    {
      OutputText("=== Search Results ===");
      for (auto& msg : m_ChatMessages)
      {
        std::string senderName = msg.isOutgoing
          ? "You"
          : GetContactName(profileId, msg.senderId);
        std::string timeStr = TimeUtil::GetTimeString(msg.timeSent, false /*p_IsExport*/);
        std::string marker = (msg.id == m_FindMsgId) ? ">>> " : "    ";
        OutputText(marker + "[" + timeStr + "] " + senderName + ": " + msg.text);
      }
    }
  }
  else
  {
    if (m_OutputJson)
    {
      OutputJson("{\"found\":false,\"messages\":[]}");
    }
    else
    {
      OutputText("No messages found matching '" + p_Query + "'.");
    }
  }
  return 0;
}

int CliMode::CmdMarkRead(const std::string& p_ChatId, const std::string& p_MsgId)
{
  std::string profileId = ResolveProfile();
  if (!m_ProfileFilter.empty() && profileId.empty()) return 1;

  if (profileId.empty())
  {
    std::cerr << "error: --profile is required when multiple profiles are configured\n";
    return 1;
  }

  auto it = m_Protocols.find(profileId);
  if (it == m_Protocols.end())
  {
    std::cerr << "error: protocol not found for profile '" << profileId << "'\n";
    return 1;
  }

  {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_MarkReadDone = false;
    m_MarkReadSuccess = false;
  }

  std::shared_ptr<MarkMessageReadRequest> request = std::make_shared<MarkMessageReadRequest>();
  request->chatId = p_ChatId;
  request->msgId = p_MsgId;
  it->second->SendRequest(request);

  // Wait for response
  {
    std::unique_lock<std::mutex> lock(m_Mutex);
    bool gotResponse = m_CondVar.wait_for(lock, std::chrono::seconds(m_TimeoutSec), [this]
    {
      return m_MarkReadDone;
    });

    if (!gotResponse)
    {
      std::cerr << "error: mark-read timed out\n";
      return 1;
    }

    if (!m_MarkReadSuccess)
    {
      std::cerr << "error: mark-read failed\n";
      return 1;
    }
  }

  // Also update the cache
  MessageCache::UpdateMessageIsRead(profileId, p_ChatId, p_MsgId, true);

  if (m_OutputJson)
  {
    OutputJson("{\"success\":true}");
  }
  else
  {
    OutputText("Message marked as read.");
  }
  return 0;
}

int CliMode::CmdWatch(const std::string& p_ChatId)
{
  {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_WatchMode = true;
    m_WatchChatFilter = p_ChatId;
  }

  // Install signal handlers for graceful shutdown
  struct sigaction sa;
  sa.sa_handler = SignalHandler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  if (!m_OutputJson)
  {
    if (p_ChatId.empty())
    {
      OutputText("=== Watching all chats (Ctrl-C to stop) ===");
    }
    else
    {
      OutputText("=== Watching chat " + p_ChatId + " (Ctrl-C to stop) ===");
    }
  }

  // Block until signal
  while (s_Running)
  {
    sleep(1);
  }

  if (!m_OutputJson)
  {
    OutputText("\nWatch stopped.");
  }
  return 0;
}

// Output helpers

void CliMode::OutputText(const std::string& p_Text)
{
  std::cout << p_Text << "\n";
}

void CliMode::OutputJson(const std::string& p_Json)
{
  std::cout << p_Json << "\n";
}

std::string CliMode::EscapeJson(const std::string& p_Str)
{
  std::string result;
  result.reserve(p_Str.size());
  for (char c : p_Str)
  {
    switch (c)
    {
      case '"': result += "\\\""; break;
      case '\\': result += "\\\\"; break;
      case '\n': result += "\\n"; break;
      case '\r': result += "\\r"; break;
      case '\t': result += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20)
        {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
          result += buf;
        }
        else
        {
          result += c;
        }
        break;
    }
  }
  return result;
}

std::string CliMode::ChatMessageToJson(const ChatMessage& p_Msg, const std::string& p_ProfileId,
                                       const std::string& p_ChatId)
{
  std::string senderName = p_Msg.isOutgoing
    ? GetSelfName(p_ProfileId)
    : GetContactName(p_ProfileId, p_Msg.senderId);

  std::string json = "{";
  json += "\"id\":\"" + EscapeJson(p_Msg.id) + "\"";
  json += ",\"chat_id\":\"" + EscapeJson(p_ChatId) + "\"";
  json += ",\"sender_id\":\"" + EscapeJson(p_Msg.senderId) + "\"";
  json += ",\"sender_name\":\"" + EscapeJson(senderName) + "\"";
  json += ",\"text\":\"" + EscapeJson(p_Msg.text) + "\"";
  json += ",\"time\":" + std::to_string(p_Msg.timeSent);
  json += ",\"outgoing\":" + std::string(p_Msg.isOutgoing ? "true" : "false");
  json += ",\"read\":" + std::string(p_Msg.isRead ? "true" : "false");
  if (!p_Msg.quotedId.empty())
  {
    json += ",\"quoted_id\":\"" + EscapeJson(p_Msg.quotedId) + "\"";
    json += ",\"quoted_text\":\"" + EscapeJson(p_Msg.quotedText) + "\"";
    json += ",\"quoted_sender\":\"" + EscapeJson(p_Msg.quotedSender) + "\"";
  }
  if (!p_Msg.fileInfo.empty())
  {
    json += ",\"file_info\":\"" + EscapeJson(p_Msg.fileInfo) + "\"";
  }
  json += "}";
  return json;
}

std::string CliMode::ChatInfoToJson(const ChatInfo& p_Info, const std::string& p_ProfileId)
{
  std::string name = GetContactName(p_ProfileId, p_Info.id);
  std::string json = "{";
  json += "\"id\":\"" + EscapeJson(p_Info.id) + "\"";
  json += ",\"profile\":\"" + EscapeJson(p_ProfileId) + "\"";
  json += ",\"name\":\"" + EscapeJson(name) + "\"";
  json += ",\"unread\":" + std::string(p_Info.isUnread ? "true" : "false");
  json += ",\"muted\":" + std::string(p_Info.isMuted ? "true" : "false");
  json += ",\"pinned\":" + std::string(p_Info.isPinned ? "true" : "false");
  json += ",\"last_message_time\":" + std::to_string(p_Info.lastMessageTime);
  json += "}";
  return json;
}

std::string CliMode::ContactInfoToJson(const ContactInfo& p_Info, const std::string& p_ProfileId)
{
  std::string json = "{";
  json += "\"id\":\"" + EscapeJson(p_Info.id) + "\"";
  json += ",\"profile\":\"" + EscapeJson(p_ProfileId) + "\"";
  json += ",\"name\":\"" + EscapeJson(p_Info.name) + "\"";
  json += ",\"phone\":\"" + EscapeJson(p_Info.phone) + "\"";
  json += ",\"is_self\":" + std::string(p_Info.isSelf ? "true" : "false");
  json += "}";
  return json;
}

std::string CliMode::GetContactName(const std::string& p_ProfileId, const std::string& p_Id)
{
  auto profIt = m_ContactInfos.find(p_ProfileId);
  if (profIt != m_ContactInfos.end())
  {
    auto contactIt = profIt->second.find(p_Id);
    if (contactIt != profIt->second.end())
    {
      if (contactIt->second.isSelf)
      {
        return "You";
      }
      if (!contactIt->second.name.empty())
      {
        return contactIt->second.name;
      }
    }
  }
  return p_Id;
}

std::string CliMode::GetSelfName(const std::string& p_ProfileId)
{
  auto it = m_Protocols.find(p_ProfileId);
  if (it != m_Protocols.end())
  {
    std::string selfId = it->second->GetSelfId();
    return GetContactName(p_ProfileId, selfId);
  }
  return "You";
}
