// climode.h
//
// Copyright (c) 2024-2026 Kristofer Berggren
// All rights reserved.
//
// nchat is distributed under the MIT license, see LICENSE for details.

#pragma once

#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "protocol.h"

class CliMode
{
public:
  CliMode();
  ~CliMode();

  void SetProfile(const std::string& p_ProfileId);
  void SetOutputJson(bool p_Json);
  void SetTimeout(int p_Seconds);
  void AddProtocol(std::shared_ptr<Protocol> p_Protocol);
  std::unordered_map<std::string, std::shared_ptr<Protocol>> GetProtocols();

  int Run(const std::string& p_Command, const std::map<std::string, std::string>& p_Options);

  void MessageHandler(std::shared_ptr<ServiceMessage> p_ServiceMessage);

private:
  bool WaitForConnection(int p_TimeoutSec);

  // Command implementations
  int CmdListChats(int p_Limit);
  int CmdListContacts();
  int CmdReadMessages(const std::string& p_ChatId, int p_Limit);
  int CmdSendMessage(const std::string& p_ChatId, const std::string& p_Text,
                     const std::string& p_FilePath);
  int CmdSearch(const std::string& p_ChatId, const std::string& p_Query);
  int CmdMarkRead(const std::string& p_ChatId, const std::string& p_MsgId);
  int CmdListProfiles();
  int CmdWatch(const std::string& p_ChatId);

  // Output helpers
  void OutputText(const std::string& p_Text);
  void OutputJson(const std::string& p_Json);
  std::string ChatMessageToJson(const ChatMessage& p_Msg, const std::string& p_ProfileId,
                                const std::string& p_ChatId);
  std::string ChatInfoToJson(const ChatInfo& p_Info, const std::string& p_ProfileId);
  std::string ContactInfoToJson(const ContactInfo& p_Info, const std::string& p_ProfileId);
  std::string EscapeJson(const std::string& p_Str);
  std::string GetContactName(const std::string& p_ProfileId, const std::string& p_Id);
  std::string GetSelfName(const std::string& p_ProfileId);

  // Profile resolution
  std::string ResolveProfile();

  // Synchronization for async protocol responses
  std::mutex m_Mutex;
  std::condition_variable m_CondVar;
  std::map<std::string, bool> m_Connected;
  bool m_AllConnected = false;

  std::string m_ProfileFilter;
  bool m_OutputJson = false;
  int m_TimeoutSec = 30;

  std::unordered_map<std::string, std::shared_ptr<Protocol>> m_Protocols;

  // Received data buffers (protected by m_Mutex)
  std::vector<std::pair<std::string, ChatInfo>> m_ChatInfos;
  std::unordered_map<std::string, std::unordered_map<std::string, ContactInfo>> m_ContactInfos;
  std::vector<ChatMessage> m_ChatMessages;
  std::string m_ChatMessagesProfileId;
  std::string m_ChatMessagesChatId;
  bool m_ResponseReceived = false;
  bool m_LastSuccess = false;

  // For send
  bool m_SendDone = false;
  bool m_SendSuccess = false;

  // For mark-read
  bool m_MarkReadDone = false;
  bool m_MarkReadSuccess = false;

  // For search/find
  bool m_FindDone = false;
  bool m_FindSuccess = false;
  std::string m_FindMsgId;

  // For watch mode
  bool m_WatchMode = false;
  std::string m_WatchChatFilter;
};
