#include <fstream>

#include "config.h"
#include "loggertypes.h"

// The lightweight OCS target needs Logged's registry but deliberately avoids
// upstream loggers.cpp, whose translation unit pulls in protocol and logfile
// implementations. Keep these definitions aligned with the fixed upstream
// implementation at commit 841d9e7be46bb968eece766aa4b6c044c7799f67.
LoggedManager::LoggedManager() = default;

void LoggedManager::add_logged(Logged* logged) {
    _idmap.push_back(logged);
}

void LoggedManager::dump_idmap() {
    std::ofstream output("idmap.txt");
    for (const Logged* logged : _idmap) {
        output << logged->get_id() << " " << logged->_name << '\n';
    }
}

LoggedManager Logged::_logged_manager;
