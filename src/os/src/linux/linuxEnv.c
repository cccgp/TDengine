/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#define _DEFAULT_SOURCE
#include "os.h"
#include "tglobal.h"

void osInit() {
#ifdef _TD_POWER_
  if (configDir[0] == 0) {
    strcpy(configDir, "/etc/power");
  }
  strcpy(tsDataDir, "/var/lib/power");
  strcpy(tsLogDir, "/var/log/power");
  strcpy(tsScriptDir, "/etc/power");
#elif (_TD_TQ_ == true)
	if (configDir[0] == 0) {
	  strcpy(configDir, "/etc/tq");
	}
	strcpy(tsDataDir, "/var/lib/tq");
	strcpy(tsLogDir, "/var/log/tq");
	strcpy(tsScriptDir, "/etc/tq");
#elif (_TD_PRO_ == true)
	if (configDir[0] == 0) {
	  strcpy(configDir, "/etc/ProDB");
	}
	strcpy(tsDataDir, "/var/lib/ProDB");
	strcpy(tsLogDir, "/var/log/ProDB");
	strcpy(tsScriptDir, "/etc/ProDB");
#else
  if (configDir[0] == 0) {
    strcpy(configDir, "/etc/taos");
  }
  strcpy(tsDataDir, "/var/lib/taos");
  strcpy(tsLogDir, "/var/log/taos");
  strcpy(tsScriptDir, "/etc/taos");
#endif

  strcpy(tsVnodeDir, "");
  strcpy(tsDnodeDir, "");
  strcpy(tsMnodeDir, "");
  strcpy(tsOsName, "Linux");
}

char* taosGetCmdlineByPID(int pid) {
  static char cmdline[1024];
  sprintf(cmdline, "/proc/%d/cmdline", pid);

  int fd = open(cmdline, O_RDONLY);
  if (fd >= 0) {
    int n = read(fd, cmdline, sizeof(cmdline) - 1);
    if (n < 0) n = 0;

    if (n > 0 && cmdline[n - 1] == '\n') --n;

    cmdline[n] = 0;

    close(fd);
  } else {
    cmdline[0] = 0;
  }

  return cmdline;
}
