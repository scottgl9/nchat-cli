// main.cpp
//
// Copyright (c) 2019-2026 Kristofer Berggren
// All rights reserved.
//
// nchat is distributed under the MIT license, see LICENSE for details.

#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <string>

#include <cassert>
#include <dlfcn.h>

#include <path.hpp>

#include "appconfig.h"
#include "apputil.h"
#include "debuginfo.h"
#include "fileutil.h"
#include "log.h"
#include "messagecache.h"
#include "profiles.h"
#include "scopeddirlock.h"
#include "status.h"
#include "sysutil.h"
#include "climode.h"
#include "ui.h"

#ifdef HAS_DUMMY
#include "duchat.h"
#endif

#ifdef HAS_TELEGRAM
#include "tgchat.h"
#endif

#ifdef HAS_WHATSAPP
#include "wmchat.h"
#endif

#ifdef HAS_SIGNAL
#include "sgchat.h"
#endif

static std::string GetFeatures();
static void RemoveProfile();
static std::shared_ptr<Protocol> SetupProfile();
static void ShowHelp();
static void ShowVersion();


class ProtocolBaseFactory
{
public:
  ProtocolBaseFactory() { }
  virtual ~ProtocolBaseFactory() { }
  virtual std::string GetName() const = 0;
  virtual std::string GetSetupMessage() const = 0;
  virtual std::shared_ptr<Protocol> Create() const = 0;
};

template<typename T>
class ProtocolFactory : public ProtocolBaseFactory
{
public:
  virtual std::string GetName() const
  {
    return T::GetName();
  }

  virtual std::string GetSetupMessage() const
  {
    return T::GetSetupMessage();
  }

  virtual std::shared_ptr<Protocol> Create() const
  {
    std::shared_ptr<T> protocol;
#ifdef HAS_DYNAMICLOAD
    std::string libPath =
      FileUtil::DirName(FileUtil::GetSelfPath()) + "/../" CMAKE_INSTALL_LIBDIR "/" + T::GetLibName() +
      FileUtil::GetLibSuffix();
    std::string createFunc = T::GetCreateFunc();
    void* handle = dlopen(libPath.c_str(), RTLD_LAZY);
    if (handle == nullptr)
    {
      LOG_ERROR("failed dlopen %s", libPath.c_str());
      const char* dlerr = dlerror();
      if (dlerr != nullptr)
      {
        LOG_ERROR("dlerror %s", dlerr);
      }

      std::cout << "Failed to load " << libPath << ", skipping profile.\n";
      return protocol;
    }

    // intentionally leak 'handle' as dlclose may cause exit crash as go routines are not explicitly managed

    T* (* CreateFunc)() = (T * (*)())dlsym(handle, createFunc.c_str());
    if (CreateFunc == nullptr)
    {
      LOG_ERROR("failed dlsym %s", createFunc.c_str());
      return protocol;
    }

    protocol.reset(CreateFunc());
#else
    protocol = std::make_shared<T>();
#endif
    return protocol;
  }
};

static std::vector<std::shared_ptr<ProtocolBaseFactory>> GetProtocolFactorys()
{
  std::vector<std::shared_ptr<ProtocolBaseFactory>> protocolFactorys =
  {
#ifdef HAS_DUMMY
    std::shared_ptr<ProtocolBaseFactory>(new ProtocolFactory<DuChat>()),
#endif
#ifdef HAS_TELEGRAM
    std::shared_ptr<ProtocolBaseFactory>(new ProtocolFactory<TgChat>()),
#endif
#ifdef HAS_WHATSAPP
    std::shared_ptr<ProtocolBaseFactory>(new ProtocolFactory<WmChat>()),
#endif
#ifdef HAS_SIGNAL
    std::shared_ptr<ProtocolBaseFactory>(new ProtocolFactory<SgChat>()),
#endif
  };

  return protocolFactorys;
}


int main(int argc, char* argv[])
{
  // Defaults
  umask(S_IRWXG | S_IRWXO);
  FileUtil::SetApplicationDir(FileUtil::GetDefaultApplicationDir());
  Log::SetVerboseLevel(Log::INFO_LEVEL);

  // Argument handling
  std::string exportDir;
  bool isKeyDump = false;
  bool isRemove = false;
  bool isSetup = false;

  // CLI mode arguments
  std::string cliCommand;
  std::map<std::string, std::string> cliOptions;
  bool cliJson = false;
  std::string cliProfile;
  int cliTimeout = 30;

  std::vector<std::string> args(argv + 1, argv + argc);
  for (auto it = args.begin(); it != args.end(); ++it)
  {
    if (((*it == "-d") || (*it == "--confdir")) && (std::distance(it + 1, args.end()) > 0))
    {
      ++it;
      FileUtil::SetApplicationDir(*it);
    }
    else if ((*it == "-e") || (*it == "--verbose"))
    {
      Log::SetVerboseLevel(Log::DEBUG_LEVEL);
    }
    else if ((*it == "-ee") || (*it == "--extra-verbose"))
    {
      Log::SetVerboseLevel(Log::TRACE_LEVEL);
    }
    else if ((*it == "-h") || (*it == "--help"))
    {
      ShowHelp();
      return 0;
    }
    else if ((*it == "-k") || (*it == "--keydump"))
    {
      isKeyDump = true;
    }
    else if ((*it == "-m") || (*it == "--devmode"))
    {
      AppUtil::SetDeveloperMode(true);
    }
    else if ((*it == "-mm") || (*it == "--extra-devmode"))
    {
      std::cout << "dev mode starting in 5 sec\n";
      sleep(5);
      AppUtil::SetDeveloperMode(true);
    }
    else if ((*it == "-r") || (*it == "--remove"))
    {
      isRemove = true;
    }
    else if ((*it == "-s") || (*it == "--setup"))
    {
      isSetup = true;
    }
    else if ((*it == "-v") || (*it == "--version"))
    {
      ShowVersion();
      return 0;
    }
    else if (((*it == "-x") || (*it == "--export")) && (std::distance(it + 1, args.end()) > 0))
    {
      ++it;
      exportDir = *it;
    }
    else if ((*it == "--cmd") && (std::distance(it + 1, args.end()) > 0))
    {
      ++it;
      cliCommand = *it;
    }
    else if ((*it == "--chat") && (std::distance(it + 1, args.end()) > 0))
    {
      ++it;
      cliOptions["chat"] = *it;
    }
    else if ((*it == "--text") && (std::distance(it + 1, args.end()) > 0))
    {
      ++it;
      cliOptions["text"] = *it;
    }
    else if ((*it == "--file") && (std::distance(it + 1, args.end()) > 0))
    {
      ++it;
      cliOptions["file"] = *it;
    }
    else if ((*it == "--query") && (std::distance(it + 1, args.end()) > 0))
    {
      ++it;
      cliOptions["query"] = *it;
    }
    else if ((*it == "--msg") && (std::distance(it + 1, args.end()) > 0))
    {
      ++it;
      cliOptions["msg"] = *it;
    }
    else if ((*it == "--limit") && (std::distance(it + 1, args.end()) > 0))
    {
      ++it;
      cliOptions["limit"] = *it;
    }
    else if ((*it == "--profile") && (std::distance(it + 1, args.end()) > 0))
    {
      ++it;
      cliProfile = *it;
    }
    else if ((*it == "--timeout") && (std::distance(it + 1, args.end()) > 0))
    {
      ++it;
      cliTimeout = std::stoi(*it);
    }
    else if (*it == "--json")
    {
      cliJson = true;
    }
    else
    {
      ShowHelp();
      return 1;
    }
  }

  bool isCliMode = !cliCommand.empty();

  bool isDirInited = false;
  static const int dirVersion = 1;
  if (!apathy::Path(FileUtil::GetApplicationDir()).exists())
  {
    FileUtil::InitDirVersion(FileUtil::GetApplicationDir(), dirVersion);
    isDirInited = true;
  }

  ScopedDirLock dirLock(FileUtil::GetApplicationDir());
  if (!dirLock.IsLocked())
  {
    std::cerr <<
      "error: unable to acquire lock for " << FileUtil::GetApplicationDir() << "\n" <<
      "       only one nchat session per account/confdir is supported.\n";
    return 1;
  }

  if (!isDirInited)
  {
    int storedVersion = FileUtil::GetDirVersion(FileUtil::GetApplicationDir());
    if (storedVersion != dirVersion)
    {
      if (isSetup)
      {
        FileUtil::InitDirVersion(FileUtil::GetApplicationDir(), dirVersion);
      }
      else
      {
        std::cerr << "error: invalid config dir content, exiting. use -s to setup nchat.\n";
        return 1;
      }
    }
  }

  // Remove profile
  if (isRemove)
  {
    RemoveProfile();
    return 0;
  }

  // Init profiles dir
  Profiles::Init();

  // Init logging
  const std::string& logPath = FileUtil::GetApplicationDir() + std::string("/log.txt");
  Log::Init(logPath);
  std::string appNameVersion = AppUtil::GetAppName(true /*p_WithVersion*/);
  LOG_INFO("%s", appNameVersion.c_str());
  std::string osArch = SysUtil::GetOsArch();
  LOG_INFO("%s", osArch.c_str());
  std::string compiler = SysUtil::GetCompiler();
  LOG_INFO("%s", compiler.c_str());
  std::string go = SysUtil::GetGo(GO_VERSION);
  LOG_INFO("%s", go.c_str());
  std::string features = GetFeatures();
  LOG_INFO("%s", features.c_str());

  // Init signal handler
  AppUtil::InitSignalHandler();

  // Run keydump if required
  if (isKeyDump)
  {
    Ui::RunKeyDump();
    return 0;
  }

  // Init app config
  AppConfig::Init();
  FileUtil::SetDownloadsDir(AppConfig::GetStr("downloads_dir"));
  static const bool isLogdumpEnabled = AppConfig::GetBool("logdump_enabled");

  // Init debug info, log last version
  DebugInfo::Init();
  const std::string versionUsed = DebugInfo::GetStr("version_used");
  if (!versionUsed.empty() && (versionUsed != AppUtil::GetAppVersion()))
  {
    LOG_INFO("last version %s", versionUsed.c_str());
  }

  // Init core dump
  static const bool isCoredumpEnabled = AppConfig::GetBool("coredump_enabled");
  if (isCoredumpEnabled)
  {
#ifndef HAS_COREDUMP
    LOG_WARNING("core dump not supported");
#else
    AppUtil::InitCoredump();
#endif
  }

  // Init message cache
  MessageCache::Init();

  // Run setup if required
  std::shared_ptr<Protocol> setupProtocol;
  if (isSetup)
  {
    setupProtocol = SetupProfile();
    if (!setupProtocol)
    {
      MessageCache::Cleanup();
      DebugInfo::Cleanup();
      AppConfig::Cleanup();
      return 1;
    }
  }

  // Init temp
  FileUtil::InitTempDir();

  // Init UI or CLI mode
  std::shared_ptr<Ui> ui;
  std::shared_ptr<CliMode> cliMode;
  std::function<void(std::shared_ptr<ServiceMessage>)> messageHandler;

  if (isCliMode)
  {
    cliMode = std::make_shared<CliMode>();
    cliMode->SetOutputJson(cliJson);
    cliMode->SetTimeout(cliTimeout);
    if (!cliProfile.empty())
    {
      cliMode->SetProfile(cliProfile);
    }
    messageHandler =
      std::bind(&CliMode::MessageHandler, std::ref(*cliMode), std::placeholders::_1);
  }
  else
  {
    ui = std::make_shared<Ui>();
    messageHandler =
      std::bind(&Ui::MessageHandler, std::ref(*ui), std::placeholders::_1);
  }

  // Set message cache message handler
  MessageCache::SetMessageHandler(messageHandler);

  // Load profile(s)
  std::string profilesDir = FileUtil::GetApplicationDir() + "/profiles";
  const std::vector<apathy::Path>& profilePaths = apathy::Path::listdir(profilesDir);
  for (auto& profilePath : profilePaths)
  {
    std::string profileId = profilePath.filename();
    if (profileId == "version") continue;

    std::stringstream ss(profileId);
    std::string protocolName;
    if ((profileId.find("_") == std::string::npos) || !std::getline(ss, protocolName, '_'))
    {
      LOG_WARNING("invalid profile name, skipping %s", profileId.c_str());
      continue;
    }

#ifndef HAS_MULTIPROTOCOL
    {
      auto currentProtocols = isCliMode ? cliMode->GetProtocols() : ui->GetProtocols();
      if (!currentProtocols.empty())
      {
        LOG_WARNING("multiple profile support not enabled, skipping %s", profileId.c_str());
        continue;
      }
    }
#endif

    if (setupProtocol && (setupProtocol->GetProfileId() == profileId))
    {
      LOG_DEBUG("adding new profile %s", profileId.c_str());
      if (isCliMode)
        cliMode->AddProtocol(setupProtocol);
      else
        ui->AddProtocol(setupProtocol);
      setupProtocol.reset();
    }
    else
    {
      bool found = false;
      std::vector<std::shared_ptr<ProtocolBaseFactory>> allProtocolFactorys = GetProtocolFactorys();
      for (auto& protocolFactory : allProtocolFactorys)
      {
        if (protocolFactory->GetName() == protocolName)
        {
          LOG_DEBUG("loading existing profile %s", profileId.c_str());
          std::shared_ptr<Protocol> protocol = protocolFactory->Create();
          if (protocol)
          {
            protocol->LoadProfile(profilesDir, profileId);
            if (isCliMode)
              cliMode->AddProtocol(protocol);
            else
              ui->AddProtocol(protocol);
            found = true;
          }
        }
      }

      if (!found)
      {
        std::string msg = "Protocol " + protocolName + " not supported";
        if (protocolName == "WhatsAppMd")
        {
          // Special logging for WhatsApp which may be auto-disabled from build if go dependency not met
          msg += " (requires Go >= " + std::string(GO_VERSION_MIN) + ")";
        }

        LOG_WARNING("%s", msg.c_str());
        std::cout << msg << "\n";
      }
    }
  }

  int cliResult = 0;
  std::unordered_map<std::string, std::shared_ptr<Protocol>> protocols;
  bool hasProtocols = false;

  if (isCliMode)
  {
    // CLI mode
    LOG_DEBUG("entering CLI mode, command=%s", cliCommand.c_str());
    protocols = cliMode->GetProtocols();
    hasProtocols = !protocols.empty();
    LOG_DEBUG("CLI mode hasProtocols=%d count=%zu", hasProtocols, protocols.size());
    if (hasProtocols)
    {
      // Login protocols (blocking, in main thread)
      std::map<std::string, std::shared_ptr<Protocol>> protocolsSorted(protocols.begin(),
                                                                       protocols.end());
      LOG_DEBUG("CLI mode logging in %zu protocols", protocolsSorted.size());
      for (auto& protocol : protocolsSorted)
      {
        protocol.second->SetMessageHandler(messageHandler);
        protocol.second->Login();
      }
      LOG_DEBUG("CLI mode protocols logged in");

      // Run the CLI command
      LOG_DEBUG("CLI mode calling Run()");
      cliResult = cliMode->Run(cliCommand, cliOptions);
      LOG_DEBUG("CLI mode Run() returned %d", cliResult);

      // Logout
      for (auto& protocol : protocolsSorted)
      {
        protocol.second->Logout();
        protocol.second->CloseProfile();
      }
    }
  }
  else
  {
    // TUI mode
    ui->Init();
    protocols = ui->GetProtocols();
    hasProtocols = !protocols.empty();
    if (hasProtocols && exportDir.empty())
    {
      // Sort protocols
      std::map<std::string, std::shared_ptr<Protocol>> protocolsSorted(protocols.begin(),
                                                                       protocols.end());

      // Connecting status
      for (auto& protocol : protocolsSorted)
      {
        Status::Set(protocol.first, Status::FlagConnecting);
      }

      // Login
      std::thread loginThread([&]
      {
        for (auto& protocol : protocolsSorted)
        {
          protocol.second->SetMessageHandler(messageHandler);
          protocol.second->Login();
        }
      });

      // Ui main loop
      ui->Run();

      // Cleanup login thread
      if (loginThread.joinable())
      {
        loginThread.join();
      }

      // Logout
      for (auto& protocol : protocolsSorted)
      {
        protocol.second->Logout();
        protocol.second->CloseProfile();
      }
    }
  }

  // Clear cache message handler
  MessageCache::SetMessageHandler(nullptr);

  // Cleanup ui
  if (ui)
  {
    ui->Cleanup();
    ui.reset();
  }
  cliMode.reset();

  // Perform export if requested
  if (!exportDir.empty())
  {
    MessageCache::Export(exportDir);
  }

  // Save last version
  DebugInfo::SetStr("version_used", AppUtil::GetAppVersion());

  // Cleanup
  FileUtil::CleanupTempDir();
  MessageCache::Cleanup();
  DebugInfo::Cleanup();
  AppConfig::Cleanup();
  Profiles::Cleanup();

  // Exit code
  int rv = 0;
  if (isCliMode)
  {
    rv = hasProtocols ? cliResult : 1;
    if (!hasProtocols)
    {
      std::cerr << "No profiles setup, exiting.\n";
    }
  }
  else if (!hasProtocols)
  {
    std::cout << "No profiles setup, exiting.\n";
    rv = 1;
  }

  LOG_INFO("exit");

  Log::Cleanup(isLogdumpEnabled);

  return rv;
}

std::string GetFeatures()
{
  std::string features =
#if defined(HAS_TELEGRAM)
    "telegram on, "
#else
    "telegram off, "
#endif

#if defined(HAS_WHATSAPP)
    "whatsapp on"
#else
    "whatsapp off"
#endif
  ;

  return features;
}

void RemoveProfile()
{
  // Show profiles
  std::string profilesDir = FileUtil::GetApplicationDir() + "/profiles";
  const std::vector<apathy::Path>& profilePaths = apathy::Path::listdir(profilesDir);
  int id = 0;
  std::map<int, std::string> idPath;
  std::cout << "Remove profile:\n";
  for (auto& profilePath : profilePaths)
  {
    std::string profileId = profilePath.filename();
    if (profileId == "version") continue;

    std::stringstream ss(profileId);
    std::string protocolName;
    if ((profileId.find("_") == std::string::npos) || !std::getline(ss, protocolName, '_'))
    {
      LOG_WARNING("invalid profile name, skipping %s", profileId.c_str());
      continue;
    }

    std::cout << id << ". " << profileId << "\n";
    idPath[id++] = profilePath.string();
  }

  std::cout << id << ". Cancel removal\n";

  size_t selectid = id;
  std::cout << "Remove profile (" << selectid << "): ";
  std::string line;
  std::getline(std::cin, line);

  if (!line.empty())
  {
    try
    {
      selectid = stoi(line);
    }
    catch (...)
    {
    }
  }

  if (!idPath.count(selectid))
  {
    std::cout << "Removal aborted, exiting." << std::endl;
    return;
  }

  std::string profilePath = idPath.at(selectid);
  LOG_TRACE("deleting %s", profilePath.c_str());
  FileUtil::RmDir(profilePath);
}

std::shared_ptr<Protocol> SetupProfile()
{
  std::shared_ptr<Protocol> rv;
  std::vector<std::shared_ptr<ProtocolBaseFactory>> protocolFactorys = GetProtocolFactorys();

  std::cout << "Protocols:" << std::endl;
  size_t idx = 0;
  for (auto it = protocolFactorys.begin(); it != protocolFactorys.end(); ++it, ++idx)
  {
    std::cout << idx << ". " << (*it)->GetName() << std::endl;
  }
  std::cout << idx << ". Exit setup" << std::endl;

  size_t selectidx = idx;
  std::cout << "Select protocol (" << selectidx << "): ";
  std::string line;
  std::getline(std::cin, line);

  if (!line.empty())
  {
    try
    {
      selectidx = stoi(line);
    }
    catch (...)
    {
    }
  }

  if (selectidx >= protocolFactorys.size())
  {
    std::cout << "Setup aborted, exiting." << std::endl;
    return rv;
  }

  std::string profileId;
  std::string profilesDir = FileUtil::GetApplicationDir() + std::string("/profiles");

#ifndef HAS_MULTIPROTOCOL
  FileUtil::RmDir(profilesDir);
  FileUtil::MkDir(profilesDir);
  Profiles::Init();
#endif

  std::string setupMessage = protocolFactorys.at(selectidx)->GetSetupMessage();
  if (!setupMessage.empty())
  {
    LOG_WARNING("%s", setupMessage.c_str());
    std::cout << setupMessage;
    sleep(3);
  }

  std::shared_ptr<Protocol> protocol = protocolFactorys.at(selectidx)->Create();
  bool setupResult = protocol && protocol->SetupProfile(profilesDir, profileId);
  if (setupResult)
  {
    std::cout << "Succesfully set up profile " << profileId << "\n";
    rv = protocol;
  }
  else
  {
    std::cout << "Setup failed\n";
    protocol->Logout();
    protocol->CloseProfile();
  }

  return rv;
}

void ShowHelp()
{
  std::cout <<
    "nchat is a terminal-based telegram / whatsapp client.\n"
    "\n"
    "Usage: nchat [OPTION]\n"
    "       nchat --cmd <command> [options]\n"
    "\n"
    "Command-line Options:\n"
    "    -d, --confdir <DIR>    use a different directory than ~/.config/nchat\n"
    "    -e, --verbose          enable verbose logging\n"
    "    -ee, --extra-verbose   enable extra verbose logging\n"
    "    -h, --help             display this help and exit\n"
    "    -k, --keydump          key code dump mode\n"
    "    -m, --devmode          developer mode\n"
    "    -r, --remove           remove chat protocol account\n"
    "    -s, --setup            set up chat protocol account\n"
    "    -v, --version          output version information and exit\n"
    "    -x, --export <DIR>     export message cache to specified dir\n"
    "\n"
    "CLI Mode Commands (non-interactive):\n"
    "    --cmd list-profiles    list configured profiles\n"
    "    --cmd list-chats       list all conversations\n"
    "    --cmd list-contacts    list all contacts\n"
    "    --cmd read             read messages from a chat\n"
    "    --cmd send             send a message\n"
    "    --cmd search           search messages in a chat\n"
    "    --cmd mark-read        mark a message as read\n"
    "    --cmd watch            stream incoming messages\n"
    "\n"
    "CLI Mode Options:\n"
    "    --chat <ID>            chat id for read/send/search/mark-read/watch\n"
    "    --text <MSG>           message text for send (or pipe via stdin)\n"
    "    --file <PATH>          file attachment path for send\n"
    "    --query <TEXT>         search query for search command\n"
    "    --msg <ID>             message id for mark-read\n"
    "    --limit <N>            limit number of results (default: 20 for read)\n"
    "    --profile <ID>         select profile (e.g. Telegram_0, WhatsAppMd_0)\n"
    "    --timeout <SEC>        connection timeout in seconds (default: 30)\n"
    "    --json                 output in JSON format\n"
    "\n"
    "CLI Mode Examples:\n"
    "    nchat --cmd list-profiles\n"
    "    nchat --cmd list-chats --json\n"
    "    nchat --cmd read --chat <id> --limit 10\n"
    "    nchat --cmd send --chat <id> --text \"hello\"\n"
    "    echo \"hello\" | nchat --cmd send --chat <id>\n"
    "    nchat --cmd search --chat <id> --query \"keyword\"\n"
    "    nchat --cmd watch --json\n"
    "\n"
    "Interactive Commands:\n"
    "    PageDn      history next page\n"
    "    PageUp      history previous page\n"
    "    Tab         next chat\n"
    "    Sh-Tab      previous chat\n"
    "    Ctrl-f      jump to unread chat\n"
    "    Ctrl-g      toggle show help bar\n"
    "    Ctrl-l      toggle show contact list\n"
    "    Ctrl-n      goto chat\n"
    "    Ctrl-p      toggle show top bar\n"
    "    Ctrl-q      quit\n"
    "    Ctrl-s      insert emoji\n"
    "    Ctrl-t      send file\n"
    "    Ctrl-x      send message\n"
    "    Ctrl-y      toggle show emojis\n"
    "    KeyUp       select message\n"
    "    Alt-d       delete/leave current chat\n"
    "    Alt-e       external editor compose\n"
    "    Alt-i       auto-compose reply\n"
    "    Alt-n       search contacts\n"
    "    Alt-t       external telephone call\n"
    "    Alt-/       find in chat\n"
    "    Alt-?       find next in chat\n"
    "    Alt-$       external spell check\n"
    "    Alt-,       decrease contact list width\n"
    "    Alt-.       increase contact list width\n"
    "\n"
    "Interactive Commands for Selected Message:\n"
    "    Ctrl-d      delete selected message\n"
    "    Ctrl-r      download attached file\n"
    "    Ctrl-v      open/view attached file\n"
    "    Ctrl-w      open link\n"
    "    Ctrl-x      send reply to selected message\n"
    "    Ctrl-z      edit selected message\n"
    "    Alt-c       copy selected message to clipboard\n"
    "    Alt-q       jump to quoted/replied message\n"
    "    Alt-r       forward selected message\n"
    "    Alt-s       add/remove reaction on selected message\n"
    "    Alt-w       external message viewer\n"
    "\n"
    "Interactive Commands for Text Input:\n"
    "    Ctrl-a      move cursor to start of line\n"
    "    Ctrl-c      clear input buffer\n"
    "    Ctrl-e      move cursor to end of line\n"
    "    Ctrl-k      delete from cursor to end of line\n"
    "    Ctrl-u      delete from cursor to start of line\n"
    "    Alt-Left    move cursor backward one word\n"
    "    Alt-Right   move cursor forward one word\n"
    "    Alt-Backsp  delete previous word\n"
    "    Alt-Delete  delete next word\n"
    "    Alt-c       copy input buffer to clipboard (if no message selected)\n"
    "    Alt-v       paste into input buffer from clipboard\n"
    "    Alt-x       cut input buffer to clipboard\n"
    "\n"
    "Report bugs at https://github.com/d99kris/nchat\n"
    "\n";
}

void ShowVersion()
{
  std::cout <<
    AppUtil::GetAppName(true /*p_WithVersion*/) << "\n"
    "\n"
    "Copyright (c) 2019-2026 Kristofer Berggren\n"
    "\n"
#ifdef HAS_SIGNAL
    "Combined distribution subject to GNU AGPL v3 license.\n"
#else
    "Combined distribution subject to MIT license.\n"
#endif
    "\n"
    "Written by Kristofer Berggren.\n";
}
