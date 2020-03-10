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
#include "taoserror.h"
#include "taosmsg.h"
#include "tast.h"
#include "textbuffer.h"
#include "tschemautil.h"
#include "tscompression.h"
#include "tskiplist.h"
#include "tsqlfunction.h"
#include "tstatus.h"
#include "ttime.h"
#include "mnode.h"
#include "mgmtAcct.h"
#include "mgmtChildTable.h"
#include "mgmtDb.h"
#include "mgmtDnode.h"
#include "mgmtGrant.h"
#include "mgmtNormalTable.h"
#include "mgmtProfile.h"
#include "mgmtShell.h"
#include "mgmtSuperTable.h"
#include "mgmtTable.h"
#include "mgmtUser.h"
#include "mgmtVgroup.h"

extern void *tsNormalTableSdb;
extern void *tsChildTableSdb;

int32_t mgmtInitTables() {
  int32_t code = mgmtInitSuperTables();
  if (code != TSDB_CODE_SUCCESS) {
    return code;
  }

  code = mgmtInitNormalTables();
  if (code != TSDB_CODE_SUCCESS) {
    return code;
  }

  code = mgmtInitChildTables();
  if (code != TSDB_CODE_SUCCESS) {
    return code;
  }

  mgmtSetVgroupIdPool();

  return TSDB_CODE_SUCCESS;
}

STableInfo* mgmtGetTable(char *tableId) {
  STableInfo *tableInfo = (STableInfo *) mgmtGetSuperTable(tableId);
  if (tableInfo != NULL) {
    return tableInfo;
  }

  tableInfo = (STableInfo *) mgmtGetNormalTable(tableId);
  if (tableInfo != NULL) {
    return tableInfo;
  }

  tableInfo = (STableInfo *) mgmtGetChildTable(tableId);
  if (tableInfo != NULL) {
    return tableInfo;
  }

  return NULL;
}

STableInfo* mgmtGetTableByPos(uint32_t dnodeIp, int32_t vnode, int32_t sid) {
  SDnodeObj *pObj = mgmtGetDnode(dnodeIp);
  if (pObj != NULL && vnode >= 0 && vnode < pObj->numOfVnodes) {
    int32_t vgId = pObj->vload[vnode].vgId;
    SVgObj *pVgroup = mgmtGetVgroup(vgId);
    if (pVgroup) {
      return pVgroup->tableList[sid];
    }
  }

  return NULL;
}

int32_t mgmtGetTableMeta(SDbObj *pDb, STableInfo *pTable, STableMeta *pMeta, bool usePublicIp) {
  if (pTable->type == TSDB_TABLE_TYPE_CHILD_TABLE) {
    mgmtGetChildTableMeta(pDb, (SChildTableObj *) pTable, pMeta, usePublicIp);
  } else if (pTable->type == TSDB_TABLE_TYPE_NORMAL_TABLE) {
    mgmtGetNormalTableMeta(pDb, (SNormalTableObj *) pTable, pMeta, usePublicIp);
  } else if (pTable->type == TSDB_TABLE_TYPE_SUPER_TABLE) {
    mgmtGetSuperTableMeta(pDb, (SSuperTableObj *) pTable, pMeta, usePublicIp);
  } else {
    mTrace("%s, uid:%" PRIu64 " table meta retrieve failed, invalid type", pTable->tableId, pTable->uid);
    return TSDB_CODE_INVALID_TABLE;
  }

  mTrace("%s, uid:%" PRIu64 " table meta is retrieved", pTable->tableId, pTable->uid);
  return TSDB_CODE_SUCCESS;
}

int32_t mgmtCreateTable(SCreateTableMsg *pCreate, int32_t contLen, void *thandle, bool isGetMeta) {
  SDbObj *pDb = mgmtGetDb(pCreate->db);
  if (pDb == NULL) {
    mError("table:%s, failed to create table, db not selected", pCreate->tableId);
    return TSDB_CODE_DB_NOT_SELECTED;
  }

  STableInfo *pTable = mgmtGetTable(pCreate->tableId);
  if (pTable != NULL) {
    if (pCreate->igExists) {
      mTrace("table:%s, table is already exist, think it success", pCreate->tableId);
      return TSDB_CODE_SUCCESS;
    } else {
      mError("table:%s, failed to create table, table already exist", pCreate->tableId);
      return TSDB_CODE_TABLE_ALREADY_EXIST;
    }
  }

  SAcctObj *pAcct = mgmtGetAcct(pDb->cfg.acct);
  assert(pAcct != NULL);

  int32_t code = mgmtCheckTableLimit(pAcct, pCreate->numOfColumns);
  if (code != TSDB_CODE_SUCCESS) {
    mError("table:%s, failed to create table, table num exceed the limit", pCreate->tableId);
    return code;
  }

  if (mgmtCheckExpired()) {
    mError("table:%s, failed to create table, grant expired", pCreate->tableId);
    return TSDB_CODE_GRANT_EXPIRED;
  }

  if (pCreate->numOfTags != 0) {
    mTrace("table:%s, start to create super table, tags:%d columns:%d",
        pCreate->tableId, pCreate->numOfTags, pCreate->numOfColumns);
    return mgmtCreateSuperTable(pDb, pCreate);
  }

  code = mgmtCheckTimeSeries(pCreate->numOfColumns);
  if (code != TSDB_CODE_SUCCESS) {
    mError("table:%s, failed to create table, timeseries exceed the limit", pCreate->tableId);
    return TSDB_CODE_SUCCESS;
  }

  SVgObj *pVgroup = mgmtGetAvailableVgroup(pDb);
  if (pVgroup == NULL) {
    mTrace("table:%s, no avaliable vgroup, start to create a new one", pCreate->tableId);
    mgmtProcessCreateVgroup(pCreate, contLen, thandle, isGetMeta);
  } else {
    mTrace("table:%s, try to create table in vgroup:%d", pCreate->tableId, pVgroup->vgId);
    mgmtProcessCreateTable(pVgroup, pCreate, contLen, thandle, isGetMeta);
  }

  return TSDB_CODE_ACTION_IN_PROGRESS;
}

int32_t mgmtDropTable(SDbObj *pDb, char *tableId, int32_t ignore) {
  STableInfo *pTable = mgmtGetTable(tableId);
  if (pTable == NULL) {
    if (ignore) {
      mTrace("table:%s, table is not exist, think it success", tableId);
      return TSDB_CODE_SUCCESS;
    } else {
      mError("table:%s, failed to create table, table not exist", tableId);
      return TSDB_CODE_INVALID_TABLE;
    }
  }

  if (mgmtCheckIsMonitorDB(pDb->name, tsMonitorDbName)) {
    mError("table:%s, failed to create table, in monitor database", tableId);
    return TSDB_CODE_MONITOR_DB_FORBIDDEN;
  }

  switch (pTable->type) {
    case TSDB_TABLE_TYPE_SUPER_TABLE:
      mTrace("table:%s, start to drop super table", tableId);
      return mgmtDropSuperTable(pDb, (SSuperTableObj *) pTable);
    case TSDB_TABLE_TYPE_CHILD_TABLE:
      mTrace("table:%s, start to drop child table", tableId);
      return mgmtDropChildTable(pDb, (SChildTableObj *) pTable);
    case TSDB_TABLE_TYPE_NORMAL_TABLE:
      mTrace("table:%s, start to drop normal table", tableId);
      return mgmtDropNormalTable(pDb, (SNormalTableObj *) pTable);
    case TSDB_TABLE_TYPE_STREAM_TABLE:
      mTrace("table:%s, start to drop stream table", tableId);
      return mgmtDropNormalTable(pDb, (SNormalTableObj *) pTable);
    default:
      mError("table:%s, invalid table type:%d", tableId, pTable->type);
      return TSDB_CODE_INVALID_TABLE;
  }
}

int32_t mgmtAlterTable(SDbObj *pDb, SAlterTableMsg *pAlter) {
  STableInfo *pTable = mgmtGetTable(pAlter->tableId);
  if (pTable == NULL) {
    return TSDB_CODE_INVALID_TABLE;
  }

  if (mgmtCheckIsMonitorDB(pDb->name, tsMonitorDbName)) {
    return TSDB_CODE_MONITOR_DB_FORBIDDEN;
  }

  if (pAlter->type == TSDB_ALTER_TABLE_ADD_TAG_COLUMN) {
    if (pTable->type == TSDB_TABLE_TYPE_SUPER_TABLE) {
      return mgmtAddSuperTableTag((SSuperTableObj *) pTable, pAlter->schema, 1);
    }
  } else if (pAlter->type == TSDB_ALTER_TABLE_DROP_TAG_COLUMN) {
    if (pTable->type == TSDB_TABLE_TYPE_SUPER_TABLE) {
      return mgmtDropSuperTableTag((SSuperTableObj *) pTable, pAlter->schema[0].name);
    }
  } else if (pAlter->type == TSDB_ALTER_TABLE_CHANGE_TAG_COLUMN) {
    if (pTable->type == TSDB_TABLE_TYPE_SUPER_TABLE) {
      return mgmtModifySuperTableTagNameByName((SSuperTableObj *) pTable, pAlter->schema[0].name, pAlter->schema[1].name);
    }
  } else if (pAlter->type == TSDB_ALTER_TABLE_UPDATE_TAG_VAL) {
    if (pTable->type == TSDB_TABLE_TYPE_CHILD_TABLE) {
      return mgmtModifyChildTableTagValueByName((SChildTableObj *) pTable, pAlter->schema[0].name, pAlter->tagVal);
    }
  } else if (pAlter->type == TSDB_ALTER_TABLE_ADD_COLUMN) {
    if (pTable->type == TSDB_TABLE_TYPE_NORMAL_TABLE) {
      return mgmtAddNormalTableColumn((SNormalTableObj *) pTable, pAlter->schema, 1);
    } else if (pTable->type == TSDB_TABLE_TYPE_SUPER_TABLE) {
      return mgmtAddSuperTableColumn((SSuperTableObj *) pTable, pAlter->schema, 1);
    } else {}
  } else if (pAlter->type == TSDB_ALTER_TABLE_DROP_COLUMN) {
    if (pTable->type == TSDB_TABLE_TYPE_NORMAL_TABLE) {
      return mgmtDropNormalTableColumnByName((SNormalTableObj *) pTable, pAlter->schema[0].name);
    } else if (pTable->type == TSDB_TABLE_TYPE_SUPER_TABLE) {
      return mgmtDropSuperTableColumnByName((SSuperTableObj *) pTable, pAlter->schema[0].name);
    } else {}
  } else {}

  return TSDB_CODE_OPS_NOT_SUPPORT;
}

void mgmtCleanUpMeters() {
  mgmtCleanUpNormalTables();
  mgmtCleanUpChildTables();
  mgmtCleanUpSuperTables();
}

int32_t mgmtGetShowTableMeta(STableMeta *pMeta, SShowObj *pShow, void *pConn) {
  SDbObj *pDb = mgmtGetDb(pShow->db);
  if (pDb == NULL) {
    return TSDB_CODE_DB_NOT_SELECTED;
  }

  int32_t cols = 0;
  SSchema *pSchema = tsGetSchema(pMeta);

  pShow->bytes[cols] = TSDB_TABLE_NAME_LEN;
  pSchema[cols].type = TSDB_DATA_TYPE_BINARY;
  strcpy(pSchema[cols].name, "table_name");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

  pShow->bytes[cols] = 8;
  pSchema[cols].type = TSDB_DATA_TYPE_TIMESTAMP;
  strcpy(pSchema[cols].name, "created_time");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

  pShow->bytes[cols] = 2;
  pSchema[cols].type = TSDB_DATA_TYPE_SMALLINT;
  strcpy(pSchema[cols].name, "columns");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

  pShow->bytes[cols] = TSDB_TABLE_NAME_LEN;
  pSchema[cols].type = TSDB_DATA_TYPE_BINARY;
  strcpy(pSchema[cols].name, "stable");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

  pMeta->numOfColumns = htons(cols);
  pShow->numOfColumns = cols;

  pShow->offset[0] = 0;
  for (int32_t i = 1; i < cols; ++i) {
    pShow->offset[i] = pShow->offset[i - 1] + pShow->bytes[i - 1];
  }

  pShow->numOfRows = pDb->numOfTables;
  pShow->rowSize   = pShow->offset[cols - 1] + pShow->bytes[cols - 1];

  return 0;
}

/*
 * remove the hole in result set
 */
static void mgmtVacuumResult(char *data, int32_t numOfCols, int32_t rows, int32_t capacity, SShowObj *pShow) {
  if (rows < capacity) {
    for (int32_t i = 0; i < numOfCols; ++i) {
      memmove(data + pShow->offset[i] * rows, data + pShow->offset[i] * capacity, pShow->bytes[i] * rows);
    }
  }
}

int32_t mgmtRetrieveShowTables(SShowObj *pShow, char *data, int32_t rows, void *pConn) {
  SDbObj *pDb = mgmtGetDb(pShow->db);
  if (pDb == NULL) return 0;

  SUserObj *pUser = mgmtGetUserFromConn(pConn);
  if (pUser == NULL) return 0;

  if (mgmtCheckIsMonitorDB(pDb->name, tsMonitorDbName)) {
    if (strcmp(pUser->user, "root") != 0 && strcmp(pUser->user, "_root") != 0 &&
        strcmp(pUser->user, "monitor") != 0) {
      return 0;
    }
  }

  int32_t numOfRows  = 0;
  int32_t numOfRead  = 0;
  int32_t cols       = 0;
  void    *pTable    = NULL;
  char    *pWrite    = NULL;
  char    prefix[20] = {0};
  SPatternCompareInfo info = PATTERN_COMPARE_INFO_INITIALIZER;

  strcpy(prefix, pDb->name);
  strcat(prefix, TS_PATH_DELIMITER);
  int32_t prefixLen = strlen(prefix);

  while (numOfRows < rows) {
    int16_t numOfColumns      = 0;
    int64_t createdTime       = 0;
    char    *tableId          = NULL;
    char    *superTableId     = NULL;
    void    *pNormalTableNode = sdbFetchRow(tsNormalTableSdb, pShow->pNode, (void **) &pTable);
    if (pTable != NULL) {
      SNormalTableObj *pNormalTable = (SNormalTableObj *) pTable;
      pShow->pNode = pNormalTableNode;
      tableId      = pNormalTable->tableId;
      superTableId = NULL;
      createdTime  = pNormalTable->createdTime;
      numOfColumns = pNormalTable->numOfColumns;
    } else {
      void *pChildTableNode = sdbFetchRow(tsChildTableSdb, pShow->pNode, (void **) &pTable);
      if (pTable != NULL) {
        SChildTableObj *pChildTable = (SChildTableObj *) pTable;
        pShow->pNode = pChildTableNode;
        tableId      = pChildTable->tableId;
        superTableId = pChildTable->superTableId;
        createdTime  = pChildTable->createdTime;
        numOfColumns = pChildTable->superTable->numOfColumns;
      } else {
        break;
      }
    }

    // not belong to current db
    if (strncmp(tableId, prefix, prefixLen)) {
      continue;
    }

    char tableName[TSDB_TABLE_NAME_LEN] = {0};
    memset(tableName, 0, tListLen(tableName));
    numOfRead++;

    // pattern compare for meter name
    extractTableName(tableId, tableName);

    if (pShow->payloadLen > 0 &&
        patternMatch(pShow->payload, tableName, TSDB_TABLE_NAME_LEN, &info) != TSDB_PATTERN_MATCH) {
      continue;
    }

    cols = 0;

    pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
    strncpy(pWrite, tableName, TSDB_TABLE_NAME_LEN);
    cols++;

    pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
    *(int64_t *) pWrite = createdTime;
    cols++;

    pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
    *(int16_t *) pWrite = numOfColumns;
    cols++;

    pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
    if (superTableId != NULL) {
      extractTableName(superTableId, pWrite);
    }
    cols++;

    numOfRows++;
  }

  pShow->numOfReads += numOfRead;
  const int32_t NUM_OF_COLUMNS = 4;

  mgmtVacuumResult(data, NUM_OF_COLUMNS, numOfRows, rows, pShow);

  return numOfRows;
}

SMDDropTableMsg *mgmtBuildRemoveTableMsg(STableInfo *pTable) {
  SMDDropTableMsg *pRemove = NULL;


  return pRemove;
}

void mgmtSetTableDirty(STableInfo *pTable, bool isDirty) {
  pTable->dirty = isDirty;
}

