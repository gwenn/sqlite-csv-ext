/*
** 2009 March 26
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains code for implementations of the CSV
** algorithms packaged as an SQLite virtual table module.
*/
#if !defined(SQLITE_CORE) || defined(SQLITE_ENABLE_CSV)

#ifndef SQLITE_CORE
  #include "sqlite3ext.h"
  SQLITE_EXTENSION_INIT1
#else
  #include "sqlite3.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SQLITE_AMALGAMATION
#include "csv.h"
#endif

#ifndef UNUSED_PARAMETER
#define UNUSED_PARAMETER(x) (void)(x)
#endif


/* 
** The CSV virtual-table types.
*/
typedef struct CSV CSV;
typedef struct CSVCursor CSVCursor;


/* 
** An CSV virtual-table object.
*/
struct CSV {
  sqlite3_vtab base;           /* Must be first */
  sqlite3 *db;                 /* Host database connection */
  char *zDb;                   /* Name of database containing CSV table */
  char *zName;                 /* Name of CSV table */ 
  char *zFile;                 /* Name of CSV file */ 
  int nBusy;                   /* Current number of users of this structure */
  FILE *f;                     /* File pointer for source CSV file */
  long offsetFirstRow;         /* ftell position of first row */
  int eof;                     /* True when at end of file */
  int maxRow;                  /* Size of zRow buffer */
  char *zRow;                  /* Buffer for current CSV row */
  char cDelim;                 /* Character to use for delimiting columns */
  int nCol;                    /* Number of columns in current row */
  int maxCol;                  /* Size of aCols array */
  char **aCols;                /* Array of parsed columns */
  int *aEscapedQuotes;         /* Number of escaped quotes for each column in aCols */
};


/* 
** An CSV cursor object.
*/
struct CSVCursor {
  sqlite3_vtab_cursor base;    /* Must be first */
  long csvpos;                 /* ftell position of current zRow */
};


/*
** Forward declarations.
*/
static int csvNext( sqlite3_vtab_cursor* pVtabCursor );
static int csvInit(
  sqlite3 *db,                        /* Database connection */
  void *pAux,                         /* Unused */
  int argc, const char *const*argv,   /* Parameters to CREATE TABLE statement */
  sqlite3_vtab **ppVtab,              /* OUT: New virtual table */
  char **pzErr,                       /* OUT: Error message, if any */
  int isCreate                        /* True for xCreate, false for xConnect */
);
static void csvReference( CSV *pCSV );
static int csvRelease( CSV *pCSV );


/* 
** Abstract out file io routines for porting 
*/
static FILE *csv_open( CSV *pCSV ){
  return fopen( pCSV->zFile, "rb" );
}
static void csv_close( CSV *pCSV ){
  if( pCSV->f ) fclose( pCSV->f );
}
static int csv_seek( CSV *pCSV, long pos ){
  return fseek( pCSV->f, pos, SEEK_SET );
}
static long csv_tell( CSV *pCSV ){
  return ftell( pCSV->f );
}


/*
** This routine reads a line of text from FILE in, stores
** the text in memory obtained from malloc() and returns a pointer
** to the text.  NULL is returned at end of file, or if malloc()
** fails.
**
** The interface is like "readline" but no command-line editing
** is done.
**
** This code was modified from existing code in shell.c of the sqlite3 CLI.
*/
static char *csv_getline( CSV *pCSV ){
  int n = 0;
  int bEol = 0;
  int bShrink = 0;
  int bQuotedCol = 0;

  /* allocate initial row buffer */
  if( pCSV->maxRow < 1 ){
    pCSV->zRow = sqlite3_malloc( 100 );
    if( pCSV->zRow ){
      pCSV->maxRow = 100;
    }
  }
  if( !pCSV->zRow ) {
      sqlite3_log(SQLITE_NOMEM, "Error while reading CSV line (1)");
      return 0;
  }

  /* read until eol */
  while( !bEol ){
    /* grow row buffer as needed */
    if( n+100>pCSV->maxRow ){
      int newSize = pCSV->maxRow*2 + 100;
      if( newSize>= sqlite3_limit(pCSV->db, SQLITE_LIMIT_LENGTH, -1) ){
        sqlite3_log(SQLITE_ERROR, "CSV row is too long (> %d)", pCSV->maxRow);
        return 0;
      }
      char *p = sqlite3_realloc(pCSV->zRow, newSize);
      if( !p ) {
        sqlite3_log(SQLITE_NOMEM, "Error while reading CSV line (2)");
        return 0;
      }
      pCSV->maxRow = newSize;
      pCSV->zRow = p;
      bShrink = -1;
    }
    if( fgets(&pCSV->zRow[n], pCSV->maxRow-n, pCSV->f)==0 ){
      if( n==0 ){
        break;
      }
      pCSV->zRow[n] = '\0';
      bEol = -1;
      break;
    }
    /* look for line delimiter */
    while( pCSV->zRow[n] ){
      if( pCSV->zRow[n]=='\"' ){
        if( bQuotedCol ) {
          if( pCSV->zRow[n+1]=='\"' ) { /* escaped */
            n++;
          }else{
            bQuotedCol = 0;
          }
        }else if( n==0 || pCSV->zRow[n-1]==pCSV->cDelim ){
          bQuotedCol = 1;
        }
      }
      n++;
    }
    /* uniform line ending */
    if( (n>0) && ((pCSV->zRow[n-1]=='\n') || (pCSV->zRow[n-1]=='\r')) && !bQuotedCol ){
      if( (n>1) && ((pCSV->zRow[n-1]=='\n') && (pCSV->zRow[n-2]=='\r')) ){
        pCSV->zRow[n-2] = '\n';
        pCSV->zRow[n-1] = '\0';
        bEol = -1;
      }else{
        pCSV->zRow[n-1] = '\n';
        pCSV->zRow[n] = '\0';
        bEol = -1;
      }
    }
  }
  if( bShrink ){ 
    pCSV->zRow = sqlite3_realloc( pCSV->zRow, n+1 ); 
    pCSV->maxRow = n+1;
  }
  return bEol ? pCSV->zRow : 0;
}


/* 
** CSV virtual table module xCreate method.
*/
static int csvCreate(
  sqlite3* db, 
  void *pAux,
  int argc, 
  const char *const *argv,
  sqlite3_vtab **ppVtab, 
  char **pzErr 
){
  return csvInit( db, pAux, argc, argv, ppVtab, pzErr, 1 );
}


/* 
** CSV virtual table module xConnect method.
*/
static int csvConnect(
  sqlite3 *db,
  void *pAux,
  int argc, const char *const*argv,
  sqlite3_vtab **ppVtab,
  char **pzErr
){
  return csvInit(db, pAux, argc, argv, ppVtab, pzErr, 0);
}


/*
** CSV virtual table module xBestIndex method.
*/
static int csvBestIndex( sqlite3_vtab *pVtab, sqlite3_index_info* info )
{
  UNUSED_PARAMETER(pVtab);
  UNUSED_PARAMETER(info);

  /* TBD */

  return SQLITE_OK;
}


/* 
** CSV virtual table module xDisconnect method.
*/
static int csvDisconnect( sqlite3_vtab *pVtab ){
  return csvRelease( (CSV *)pVtab );
}


/* 
** CSV virtual table module xDestroy method.
*/
static int csvDestroy( sqlite3_vtab *pVtab ){
  return csvDisconnect( pVtab );
}


/* 
** CSV virtual table module xOpen method.
*/
static int csvOpen( sqlite3_vtab *pVtab, sqlite3_vtab_cursor **ppVtabCursor ){
  int rc = SQLITE_NOMEM;
  CSVCursor *pCsr;

  /* create a new cursor object */
  pCsr = (CSVCursor *)sqlite3_malloc(sizeof(CSVCursor));
  if( pCsr ){
    memset(pCsr, 0, sizeof(CSVCursor));
    pCsr->base.pVtab = pVtab;
    rc = SQLITE_OK;
  }
  *ppVtabCursor = (sqlite3_vtab_cursor *)pCsr;

  return rc;
}


/* 
** CSV virtual table module xClose method.
*/
static int csvClose( sqlite3_vtab_cursor *pVtabCursor ){
  CSVCursor *pCsr = (CSVCursor *)pVtabCursor;

  sqlite3_free(pCsr);

  return SQLITE_OK;
}


/* 
** CSV virtual table module xFilter method.
*/
static int csvFilter(
  sqlite3_vtab_cursor *pVtabCursor, 
  int idxNum, const char *idxStr,
  int argc, sqlite3_value **argv
){
  CSV *pCSV = (CSV *)pVtabCursor->pVtab;
  int rc;

  UNUSED_PARAMETER(idxNum);
  UNUSED_PARAMETER(idxStr);
  UNUSED_PARAMETER(argc);
  UNUSED_PARAMETER(argv);

  csvReference( pCSV );

  /* seek back to start of first zRow */
  pCSV->eof = 0;
  csv_seek( pCSV, pCSV->offsetFirstRow );
  /* read and parse next line */
  rc = csvNext( pVtabCursor );

  csvRelease( pCSV );

  return rc;
}


/* 
** CSV virtual table module xNext method.
*/
static int csvNext( sqlite3_vtab_cursor* pVtabCursor ){
  CSV *pCSV = (CSV *)pVtabCursor->pVtab;
  CSVCursor *pCsr = (CSVCursor *)pVtabCursor;
  int nCol = 0;
  char *s;
  char zDelims[3] = ",\n";
  char cDelim; /* char that delimited current col */

  if( pCSV->eof ){
    return SQLITE_ERROR;
  }

  /* update the cursor */
  pCsr->csvpos = csv_tell( pCSV );

  /* read the next row of data */
  s = csv_getline( pCSV );
  if( !s ){
    /* and error or eof occured */
    pCSV->eof = -1;
    return SQLITE_OK;
  }

  /* allocate initial space for the column pointers */
  if( pCSV->maxCol < 1 ){
    /* take a guess */
    int maxCol = (int)(strlen(pCSV->zRow) / 5 + 1);
    pCSV->aCols = (char **)sqlite3_malloc( sizeof(char*) * maxCol );
    pCSV->aEscapedQuotes = (int *)sqlite3_malloc( sizeof(int) * maxCol );
    if( pCSV->aCols ){
      pCSV->maxCol = maxCol;
    }
  }
  if( !pCSV->aCols || !pCSV->aEscapedQuotes ) return SQLITE_NOMEM;

  /* add custom delim character */
  zDelims[0] = pCSV->cDelim;

  /* parse the zRow into individual columns */
  do{
    /* if it begins with a quote, assume it's a quoted col */
    if( *s=='\"' ){
      s++;  /* skip quote */
      pCSV->aCols[nCol] = s; /* save pointer for this col */
      pCSV->aEscapedQuotes[nCol] = 0;
      /* find closing quote */
      while( 1 ){
        s = strchr(s, '\"');
        if( !s ){
          /* no closing quote */
          pCSV->eof = -1;
          return SQLITE_ERROR;
        }else if ( *(s+1)=='\"' ){
          /* escaped quote */
          pCSV->aEscapedQuotes[nCol]++;
          s+=2;
        }else{
          break;
        }
      }
      *s = '\0'; /* null terminate this col */
      /* fall through and look for following ",\n" */
      s++;
    }else{
      pCSV->aCols[nCol] = s; /* save pointer for this col */
      pCSV->aEscapedQuotes[nCol] = 0;
    }
    s = strpbrk(s, zDelims);
    if( !s ){
      /* no col delimiter */
      pCSV->eof = -1;
      return SQLITE_ERROR;
    }
    // TODO Maybe we should treat empty string as null value (if an option is
    // activated).
    cDelim = *s;
    /* null terminate the column by overwriting the delimiter */
    *s = '\0';
    nCol++;
    /* if end of zRow, stop parsing cols */
    if( cDelim == '\n' ) break;
    /* move to start of next col */
    s++; /* skip delimiter */

    if(nCol >= pCSV->maxCol ){
      if( nCol>=sqlite3_limit(pCSV->db, SQLITE_LIMIT_COLUMN, -1) ){
        return SQLITE_ERROR;
      }
      /* we need to grow our col pointer array */
      char **p = (char **)sqlite3_realloc( pCSV->aCols, sizeof(char*) * (nCol+5) );
      if( !p ){
        /* out of memory */
        return SQLITE_ERROR;
      }
      pCSV->maxCol = nCol + 5;
      pCSV->aCols = p;
      int *p1 = (int *)sqlite3_realloc( pCSV->aEscapedQuotes, sizeof(int) * (nCol+5) );
      if( !p1 ){
        /* out of memory */
        return SQLITE_ERROR;
      }
      pCSV->aEscapedQuotes = p1;
    }

  }while( *s );

  pCSV->nCol = nCol;
  return SQLITE_OK;
}


/*
** CSV virtual table module xEof method.
**
** Return non-zero if the cursor does not currently point to a valid 
** record (i.e if the scan has finished), or zero otherwise.
*/
static int csvEof( sqlite3_vtab_cursor *pVtabCursor )
{
  CSV *pCSV = (CSV *)pVtabCursor->pVtab;

  return pCSV->eof;
}


/* 
** CSV virtual table module xColumn method.
*/
static int csvColumn(sqlite3_vtab_cursor *pVtabCursor, sqlite3_context *ctx, int i){
  CSV *pCSV = (CSV *)pVtabCursor->pVtab;

  if( i<0 || i>=pCSV->nCol ){
    sqlite3_result_null( ctx );
  }else{
    // TODO SQLite uses dynamic typing...
    const char *col = pCSV->aCols[i];
    if( !col ){
      sqlite3_result_null( ctx );
    }else if( pCSV->aEscapedQuotes[i] ){
      char *z;

      int nByte = (int)(strlen(col) - pCSV->aEscapedQuotes[i] + 1);
      if( nByte>sqlite3_limit(pCSV->db, SQLITE_LIMIT_LENGTH, -1) ){
        sqlite3_result_error_toobig( ctx );
        z = 0;
      }else{
        z = sqlite3_malloc( nByte );
        if( !z ){
          sqlite3_result_error_nomem( ctx );
        }
      }
      if( z ){
        int j,k;
        for(j=0, k=0; col[j]; j++){
          z[k++] = col[j];
          if( col[j]=='\"' ){
            /* unescape quote */
            j++;
          }
        }
        z[k] = 0;
        sqlite3_result_text( ctx, z, k, sqlite3_free ); // FIXME sqlite3_result_int64/double
      }
    }else{
      sqlite3_result_text( ctx, col, -1, SQLITE_TRANSIENT ); // FIXME sqlite3_result_int64/double
    }
  }

  return SQLITE_OK;
}


/* 
** CSV virtual table module xRowid method.
** We probably should store a hidden table
** mapping rowid's to csvpos.
*/
static int csvRowid( sqlite3_vtab_cursor* pVtabCursor, sqlite3_int64 *pRowid ){
  CSVCursor *pCsr = (CSVCursor *)pVtabCursor;

  *pRowid = pCsr->csvpos;

  return SQLITE_OK;
}


static sqlite3_module csvModule = {
  0,                        /* iVersion */
  csvCreate,                /* xCreate - create a table */
  csvConnect,               /* xConnect - connect to an existing table */
  csvBestIndex,             /* xBestIndex - Determine search strategy */
  csvDisconnect,            /* xDisconnect - Disconnect from a table */
  csvDestroy,               /* xDestroy - Drop a table */
  csvOpen,                  /* xOpen - open a cursor */
  csvClose,                 /* xClose - close a cursor */
  csvFilter,                /* xFilter - configure scan constraints */
  csvNext,                  /* xNext - advance a cursor */
  csvEof,                   /* xEof */
  csvColumn,                /* xColumn - read data */
  csvRowid,                 /* xRowid - read data */
  0,                        /* xUpdate - write data */
  0,                        /* xBegin - begin transaction */
  0,                        /* xSync - sync transaction */
  0,                        /* xCommit - commit transaction */
  0,                        /* xRollback - rollback transaction */
  0,                        /* xFindFunction - function overloading */
  0                         /* xRename - rename the table */
};


/*
** Increment the CSV reference count.
*/
static void csvReference( CSV *pCSV ){
  pCSV->nBusy++;
}


/*
** Decrement the CSV reference count. When the reference count reaches
** zero the structure is deleted.
*/
static int csvRelease( CSV *pCSV ){
  pCSV->nBusy--;
  if( pCSV->nBusy<1 ){

    /* finalize any prepared statements here */

    csv_close( pCSV );
    if( pCSV->zRow ) sqlite3_free( pCSV->zRow );
    if( pCSV->aCols ) sqlite3_free( pCSV->aCols );
    if( pCSV->aEscapedQuotes ) sqlite3_free( pCSV->aEscapedQuotes );
    sqlite3_free( pCSV );
  }
  return 0;
}


/* 
** This function is the implementation of both the xConnect and xCreate
** methods of the CSV virtual table.
**
**   argv[0]   -> module name
**   argv[1]   -> database name
**   argv[2]   -> table name
**   argv[3]   -> csv file name
**   argv[4]   -> custom delimiter
**   argv[5]   -> optional:  use header row for column names
**
** TODO
**   File encoding problem
**   Column/Cell type (in declaration and in result)
*/
static int csvInit(
  sqlite3 *db,                        /* Database connection */
  void *pAux,                         /* Unused */
  int argc, const char *const*argv,   /* Parameters to CREATE TABLE statement */
  sqlite3_vtab **ppVtab,              /* OUT: New virtual table */
  char **pzErr,                       /* OUT: Error message, if any */
  int isCreate                        /* True for xCreate, false for xConnect */
){
  int rc = SQLITE_OK;
  int i;
  CSV *pCSV;
  char *zSql;
  char cDelim = ',';       /* Default col delimiter */
  int bUseHeaderRow = 0;   /* Default to not use zRow headers */
  size_t nDb;              /* Length of string argv[1] */
  size_t nName;            /* Length of string argv[2] */
  size_t nFile;            /* Length of string argv[3] */
  CSVCursor csvCsr;        /* Used for calling csvNext */

  const char *aErrMsg[] = {
    0,                                                    /* 0 */
    "No CSV file specified",                              /* 1 */
    "Error opening CSV file: '%s'",                       /* 2 */
    "No columns found",                                   /* 3 */
    "No column name found",                               /* 4 */
    "Out of memory",                                      /* 5 */
  };

  UNUSED_PARAMETER(pAux);
  UNUSED_PARAMETER(isCreate);

  if( argc < 4 ){
    *pzErr = sqlite3_mprintf("%s", aErrMsg[1]);
    return SQLITE_ERROR;
  }

  /* allocate space for the virtual table object */
  nDb = strlen(argv[1]);
  nName = strlen(argv[2]);
  nFile = strlen(argv[3]);
  pCSV = (CSV *)sqlite3_malloc( (int)(sizeof(CSV)+nDb+nName+nFile+3) );
  if( !pCSV ){
    /* out of memory */
    *pzErr = sqlite3_mprintf("%s", aErrMsg[5]);
    return SQLITE_NOMEM;
  }

  /* intialize virtual table object */
  memset(pCSV, 0, sizeof(CSV)+nDb+nName+nFile+3);
  pCSV->db = db;
  pCSV->nBusy = 1;
  pCSV->base.pModule = &csvModule;
  pCSV->cDelim = cDelim;
  pCSV->zDb = (char *)&pCSV[1];
  pCSV->zName = &pCSV->zDb[nDb+1];
  pCSV->zFile = &pCSV->zName[nName+1];
  memcpy(pCSV->zDb, argv[1], nDb);
  memcpy(pCSV->zName, argv[2], nName);

  /* pull out name of csv file (remove quotes) */
  if( argv[3][0] == '\'' ){
    memcpy( pCSV->zFile, argv[3]+1, nFile-2 );
    pCSV->zFile[nFile-2] = '\0';
  }else{
    memcpy( pCSV->zFile, argv[3], nFile );
  }

  /* if a custom delimiter specified, pull it out */
  if( argc > 4 ){
    if( argv[4][0] == '\'' ){
      pCSV->cDelim = argv[4][1];
    }else{
      pCSV->cDelim = argv[4][0];
    }
  }

  /* should the header zRow be used */
  if( argc > 5 ){
    if( !strcmp(argv[5], "USE_HEADER_ROW") ){
      bUseHeaderRow = -1;
    }
  }

  /* open the source csv file */
  pCSV->f = csv_open( pCSV );
  if( !pCSV->f ){
    *pzErr = sqlite3_mprintf(aErrMsg[2], pCSV->zFile);
    csvRelease( pCSV );
    return SQLITE_ERROR;
  }

  /* Read first zRow to obtain column names/number */
  csvCsr.base.pVtab = (sqlite3_vtab *)pCSV;
  rc = csvNext( (sqlite3_vtab_cursor *)&csvCsr );
  if( (SQLITE_OK!=rc) || (pCSV->nCol<=0) ){
    *pzErr = sqlite3_mprintf("%s", aErrMsg[3]);
    csvRelease( pCSV );
    return SQLITE_ERROR;
  }
  if( bUseHeaderRow ){
    pCSV->offsetFirstRow = csv_tell( pCSV );
  }

  /* Create the underlying relational database schema. If
  ** that is successful, call sqlite3_declare_vtab() to configure
  ** the csv table schema.
  */
  zSql = sqlite3_mprintf("CREATE TABLE x(");
  for(i=0; zSql && i<pCSV->nCol; i++){
    const char *zTail = (i+1<pCSV->nCol) ? ", " : ");";
    char *zTmp = zSql;
    if( bUseHeaderRow ){
      const char *zCol = pCSV->aCols[i];
      if( !zCol ){
        *pzErr = sqlite3_mprintf("%s", aErrMsg[4]);
        sqlite3_free(zSql);
        csvRelease( pCSV );
        return SQLITE_ERROR;
      }
      zSql = sqlite3_mprintf("%s\"%s\"%s", zTmp, zCol, zTail); // FIXME Column type (INT/REAL/TEXT)
    }else{
      zSql = sqlite3_mprintf("%scol%d%s", zTmp, i+1, zTail); // FIXME Column type (INT/REAL/TEXT)
    }
    sqlite3_free(zTmp);
  }
  if( !zSql ){
    *pzErr = sqlite3_mprintf("%s", aErrMsg[5]);
    csvRelease( pCSV );
    return SQLITE_NOMEM;
  }

  rc = sqlite3_declare_vtab( db, zSql );
  sqlite3_free(zSql);
  if( SQLITE_OK != rc ){
    *pzErr = sqlite3_mprintf("%s", sqlite3_errmsg(db));
    csvRelease( pCSV );
    return SQLITE_ERROR;
  }

  *ppVtab = (sqlite3_vtab *)pCSV;
  *pzErr  = NULL;
  return SQLITE_OK;
}


/*
** Register the CSV module with database handle db. This creates the
** virtual table module "csv".
*/
int sqlite3CsvInit(sqlite3 *db){
  int rc = SQLITE_OK;

  if( rc==SQLITE_OK ){
    void *c = (void *)NULL;
    rc = sqlite3_create_module_v2(db, "csv", &csvModule, c, 0);
  }

  return rc;
}


#if !SQLITE_CORE
/*
** Support auto-extension loading.
*/
int sqlite3_extension_init(
  sqlite3 *db,
  char **pzErrMsg,
  const sqlite3_api_routines *pApi
){
  SQLITE_EXTENSION_INIT2(pApi)
  return sqlite3CsvInit(db);
}
#endif


#endif
