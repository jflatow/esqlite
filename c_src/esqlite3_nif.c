/*
 * Copyright 2011 Maas-Maarten Zeeman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
*/

/*
 * sqlite3_nif -- an erlang sqlite nif.
*/

#include <erl_nif.h>
#include <string.h>
#include <unistd.h>

#include <stdio.h> /* for debugging */

#include "queue.h"
#include "sqlite3.h"

#define MAX_ATOM_LENGTH 255 /* from atom.h, not exposed in erlang include */
#define MAX_PATHNAME 512 /* unfortunately not in sqlite.h. */

static ErlNifResourceType *esqlite_connection_type = NULL;
static ErlNifResourceType *esqlite_statement_type = NULL;

/* database connection context */
typedef struct {
     ErlNifTid tid;
     ErlNifThreadOpts* opts;

     sqlite3 *db;
     queue *commands;

     int alive;
} esqlite_connection;

/* prepared statement */
typedef struct {
    esqlite_connection *connection;
    sqlite3_stmt *statement;
} esqlite_statement;


typedef enum {
     cmd_unknown,
     cmd_open,
     cmd_exec,
     cmd_prepare,
     cmd_bind,
     cmd_step,
     cmd_column_names,
     cmd_close,
     cmd_stop
} command_type;

typedef struct {
     command_type type;

     ErlNifEnv *env;
     ERL_NIF_TERM ref;
     ErlNifPid pid;
     ERL_NIF_TERM arg;
     sqlite3_stmt *stmt;
} esqlite_command;

static ERL_NIF_TERM
make_atom(ErlNifEnv *env, const char *atom_name)
{
     ERL_NIF_TERM atom;

     if(enif_make_existing_atom(env, atom_name, &atom, ERL_NIF_LATIN1))
	  return atom;

     return enif_make_atom(env, atom_name);
}

static ERL_NIF_TERM
make_ok_tuple(ErlNifEnv *env, ERL_NIF_TERM value)
{
     return enif_make_tuple2(env, make_atom(env, "ok"), value);
}

static ERL_NIF_TERM
make_error_tuple(ErlNifEnv *env, const char *reason)
{
     return enif_make_tuple2(env, make_atom(env, "error"), make_atom(env, reason));
}

static ERL_NIF_TERM
make_sqlite3_error_tuple(ErlNifEnv *env, const char *msg)
{
     return enif_make_tuple2(env, make_atom(env, "error"),
			     enif_make_tuple2(env, make_atom(env, "sqlite3_error"),
					      enif_make_string(env, msg, ERL_NIF_LATIN1)));
}

static void
command_destroy(void *obj)
{
     esqlite_command *cmd = (esqlite_command *) obj;

     if(cmd->env != NULL)
	  enif_free_env(cmd->env);

     enif_free(cmd);
}

static esqlite_command *
command_create()
{
     esqlite_command *cmd = (esqlite_command *) enif_alloc(sizeof(esqlite_command));
     if(cmd == NULL)
	  return NULL;

     cmd->env = enif_alloc_env();
     if(cmd->env == NULL) {
	  command_destroy(cmd);
	  return NULL;
     }

     cmd->type = cmd_unknown;
     cmd->ref = 0;
     cmd->arg = 0;
     cmd->stmt = NULL;

     return cmd;
}

/*
 *
 */
static void
destruct_esqlite_connection(ErlNifEnv *env, void *arg)
{
     esqlite_connection *db = (esqlite_connection *) arg;
     esqlite_command *cmd = command_create();

     /* Send the stop command
      */
     cmd->type = cmd_stop;
     queue_push(db->commands, cmd);
     queue_send(db->commands, cmd);

     /* Wait for the thread to finish
      */
     enif_thread_join(db->tid, NULL);
     enif_thread_opts_destroy(db->opts);

     /* The thread has finished... now remove the command queue, and close
      * the datbase (if it was still open).
      */
     queue_destroy(db->commands);

     if(db->db)
	  sqlite3_close(db->db);
}

static void
destruct_esqlite_statement(ErlNifEnv *env, void *arg)
{
     esqlite_statement *stmt = (esqlite_statement *) arg;

     if(stmt->statement) {
	  sqlite3_finalize(stmt->statement);
	  stmt->statement = NULL;
     }

     enif_release_resource(stmt->connection);
}

static ERL_NIF_TERM
do_open(ErlNifEnv *env, esqlite_connection *db, const ERL_NIF_TERM arg)
{
     char filename[MAX_PATHNAME];
     unsigned int size;
     int rc;
     ERL_NIF_TERM error;

     size = enif_get_string(env, arg, filename, MAX_PATHNAME, ERL_NIF_LATIN1);
     if(size <= 0)
	  return make_error_tuple(env, "invalid_filename");

     /* Open the database.
      */
     rc = sqlite3_open(filename, &db->db);
     if(rc != SQLITE_OK) {
	  error = make_sqlite3_error_tuple(env, sqlite3_errmsg(db->db));
	  sqlite3_close(db->db);
	  db->db = NULL;

	  return error;
     }

     return make_atom(env, "ok");
}

/*
 */
static ERL_NIF_TERM
do_exec(ErlNifEnv *env, esqlite_connection *conn, const ERL_NIF_TERM arg)
{
     ErlNifBinary bin;
     int rc;

     enif_inspect_iolist_as_binary(env, arg, &bin);

     rc = sqlite3_exec(conn->db, (char *) bin.data, NULL, NULL, NULL);
     if(rc != SQLITE_OK)
	  return make_sqlite3_error_tuple(env, sqlite3_errmsg(conn->db));

     return make_atom(env, "ok");
}

/*
 */
static ERL_NIF_TERM
do_prepare(ErlNifEnv *env, esqlite_connection *conn, const ERL_NIF_TERM arg)
{
     ErlNifBinary bin;
     esqlite_statement *stmt;
     ERL_NIF_TERM esqlite_stmt;
     const char *tail;
     int rc;
     int retries = 0;

     enif_inspect_iolist_as_binary(env, arg, &bin);

     stmt = enif_alloc_resource(esqlite_statement_type, sizeof(esqlite_statement));
     if(!stmt)
	  return make_error_tuple(env, "no_memory");

     do {
       rc = sqlite3_prepare_v2(conn->db, (char *) bin.data, bin.size, &(stmt->statement), &tail);
       usleep(retries * 100);
     } while (rc == SQLITE_BUSY && retries++ < 100);
     if(rc != SQLITE_OK)
	  return make_sqlite3_error_tuple(env, sqlite3_errmsg(conn->db));

     enif_keep_resource(conn);
     stmt->connection = conn;

     esqlite_stmt = enif_make_resource(env, stmt);
     enif_release_resource(stmt);

     return make_ok_tuple(env, esqlite_stmt);
}

static int
bind_cell(ErlNifEnv *env, const ERL_NIF_TERM cell, sqlite3_stmt *stmt, unsigned int i)
{
     int the_int;
     double the_double;
     char the_atom[MAX_ATOM_LENGTH+1];
     ErlNifBinary the_blob;

     if(enif_get_int(env, cell, &the_int))
	  return sqlite3_bind_int(stmt, i, the_int);

     if(enif_get_double(env, cell, &the_double))
	  return sqlite3_bind_double(stmt, i, the_double);

     if(enif_get_atom(env, cell, the_atom, sizeof(the_atom), ERL_NIF_LATIN1)) {
	  if(strcmp("undefined", the_atom) == 0) {
	       return sqlite3_bind_null(stmt, i);
	  }

	  return sqlite3_bind_text(stmt, i, the_atom, strlen(the_atom), SQLITE_TRANSIENT);
     }

     if(enif_inspect_iolist_as_binary(env, cell, &the_blob)) {
	  /* Bind lists which have the same length as the binary as text */
	  if(enif_is_list(env, cell) && (strlen((char *) the_blob.data) == the_blob.size)) {
	       return sqlite3_bind_text(stmt, i, (char *) the_blob.data, the_blob.size, SQLITE_TRANSIENT);
	  }

	  return sqlite3_bind_blob(stmt, i, the_blob.data, the_blob.size, SQLITE_TRANSIENT);
     }

     return -1;
}

static ERL_NIF_TERM
do_bind(ErlNifEnv *env, sqlite3 *db, sqlite3_stmt *stmt, const ERL_NIF_TERM arg)
{
     int parameter_count = sqlite3_bind_parameter_count(stmt);
     int i, is_list, r;
     ERL_NIF_TERM list, head, tail;
     unsigned int list_length;

     is_list = enif_get_list_length(env, arg, &list_length);
     if(!is_list)
	  return make_error_tuple(env, "bad_arg_list");
     if(parameter_count != list_length)
	  return make_error_tuple(env, "args_wrong_length");

     sqlite3_reset(stmt);

     list = arg;
     for(i=0; i < list_length; i++) {
	  enif_get_list_cell(env, list, &head, &tail);
	  r = bind_cell(env, head, stmt, i+1);
	  if(r == -1)
	       return make_error_tuple(env, "wrong_type");
	  if(r != SQLITE_OK)
	       return make_sqlite3_error_tuple(env, sqlite3_errmsg(db));
	  list = tail;
     }

     return make_atom(env, "ok");
}

static ERL_NIF_TERM
make_binary(ErlNifEnv *env, const void *bytes, unsigned int size)
{
     ErlNifBinary blob;
     ERL_NIF_TERM term;

     if(!enif_alloc_binary(size, &blob)) {
	  /* TODO: fix this */
	  return make_atom(env, "error");
     }

     memcpy(blob.data, bytes, size);
     term = enif_make_binary(env, &blob);
     enif_release_binary(&blob);

     return term;
}

static ERL_NIF_TERM
make_cell(ErlNifEnv *env, sqlite3_stmt *statement, unsigned int i)
{
     int type = sqlite3_column_type(statement, i);

     switch(type) {
     case SQLITE_INTEGER:
	  return enif_make_int(env, sqlite3_column_int(statement, i));
     case SQLITE_FLOAT:
	  return enif_make_double(env, sqlite3_column_double(statement, i));
     case SQLITE_BLOB:
	  return make_binary(env, sqlite3_column_blob(statement, i), sqlite3_column_bytes(statement, i));
     case SQLITE_NULL:
	  return make_atom(env, "undefined");
     case SQLITE_TEXT:
	  /* TODO, make some tests to see what happens when you insert a utf-8 string */
	  return enif_make_string(env, (char *) sqlite3_column_text(statement, i), ERL_NIF_LATIN1);
     default:
	  return make_atom(env, "should_not_happen");
     }
}

static ERL_NIF_TERM
make_row(ErlNifEnv *env, sqlite3_stmt *statement)
{
     int i, size;
     ERL_NIF_TERM *array;
     ERL_NIF_TERM row;

     size = sqlite3_column_count(statement);
     array = (ERL_NIF_TERM *) malloc(sizeof(ERL_NIF_TERM)*size);

     if(!array)
	  return make_error_tuple(env, "no_memory");

     for(i = 0; i < size; i++)
	  array[i] = make_cell(env, statement, i);

     row = enif_make_tuple_from_array(env, array, size);
     free(array);
     return row;
}

static ERL_NIF_TERM
do_step(ErlNifEnv *env, sqlite3_stmt *stmt)
{
     int rc = sqlite3_step(stmt);

     if(rc == SQLITE_DONE)
	  return make_atom(env, "$done");
     if(rc == SQLITE_BUSY)
	  return make_atom(env, "$busy");
     if(rc == SQLITE_ROW)
	  return make_row(env, stmt);

     return make_error_tuple(env, "unexpected_return_value");
}

static ERL_NIF_TERM
do_column_names(ErlNifEnv *env, sqlite3_stmt *stmt)
{
     int i, size;
     const char *name;
     ERL_NIF_TERM *array;
     ERL_NIF_TERM column_names;

     size = sqlite3_column_count(stmt);
     array = (ERL_NIF_TERM *) malloc(sizeof(ERL_NIF_TERM) * size);

     if(!array)
	  return make_error_tuple(env, "no_memory");

     for(i = 0; i < size; i++) {
	  name = sqlite3_column_name(stmt, i);
	  array[i] = make_atom(env, name);
     }

     column_names = enif_make_tuple_from_array(env, array, size);
     free(array);
     return column_names;
}

static ERL_NIF_TERM
do_close(ErlNifEnv *env, esqlite_connection *conn, const ERL_NIF_TERM arg)
{
     int rc;

     rc = sqlite3_close(conn->db);
     if(rc != SQLITE_OK)
	  return make_sqlite3_error_tuple(env, sqlite3_errmsg(conn->db));

     conn->db = NULL;
     return make_atom(env, "ok");
}

static ERL_NIF_TERM
evaluate_command(esqlite_command *cmd, esqlite_connection *conn)
{
     if(!conn->db)
	  make_error_tuple(cmd->env, "database_not_open");

     switch(cmd->type) {
     case cmd_open:
	  return do_open(cmd->env, conn, cmd->arg);
     case cmd_exec:
	  return do_exec(cmd->env, conn, cmd->arg);
     case cmd_prepare:
	  return do_prepare(cmd->env, conn, cmd->arg);
     case cmd_step:
	  return do_step(cmd->env, cmd->stmt);
     case cmd_bind:
	  return do_bind(cmd->env, conn->db, cmd->stmt, cmd->arg);
     case cmd_column_names:
	  return do_column_names(cmd->env, cmd->stmt);
     case cmd_close:
	  return do_close(cmd->env, conn, cmd->arg);
     default:
	  return make_error_tuple(cmd->env, "invalid_command");
     }
}

static ERL_NIF_TERM
make_answer(esqlite_command *cmd, ERL_NIF_TERM answer)
{
     return enif_make_tuple2(cmd->env, cmd->ref, answer);
}

static void *
esqlite_connection_run(void *arg)
{
     esqlite_connection *db = (esqlite_connection *) arg;
     esqlite_command *cmd;
     int continue_running = 1;

     db->alive = 1;

     while(continue_running) {
	  cmd = queue_pop(db->commands);

	  if(cmd->type == cmd_stop)
	       continue_running = 0;
	  else
	       enif_send(NULL, &cmd->pid, cmd->env, make_answer(cmd, evaluate_command(cmd, db)));

	  command_destroy(cmd);
     }

     db->alive = 0;
     return NULL;
}

/*
 * Start the processing thread
 */
static ERL_NIF_TERM
esqlite_start(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
     esqlite_connection *conn;
     ERL_NIF_TERM db_conn;

     /* Initialize the resource */
     conn = enif_alloc_resource(esqlite_connection_type, sizeof(esqlite_connection));
     if(!conn)
	  return make_error_tuple(env, "no_memory");

     conn->db = NULL;

     /* Create command queue */
     conn->commands = queue_create();
     if(!conn->commands) {
	  enif_release_resource(conn);
	  return make_error_tuple(env, "command_queue_create_failed");
     }

     /* Start command processing thread */
     conn->opts = enif_thread_opts_create("esqldb_thread_opts");
     if(enif_thread_create("esqlite_connection", &conn->tid, esqlite_connection_run, conn, conn->opts) != 0) {
	  enif_release_resource(conn);
	  return make_error_tuple(env, "thread_create_failed");
     }

     db_conn = enif_make_resource(env, conn);
     enif_release_resource(conn);

     return make_ok_tuple(env, db_conn);
}

/*
 * Open the database
 */
static ERL_NIF_TERM
esqlite_open(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
     esqlite_connection *db;
     esqlite_command *cmd = NULL;
     ErlNifPid pid;

     if(argc != 4)
	  return enif_make_badarg(env);

     if(!enif_get_resource(env, argv[0], esqlite_connection_type, (void **) &db))
	  return enif_make_badarg(env);

     if(!enif_is_ref(env, argv[1]))
	  return make_error_tuple(env, "invalid_ref");

     if(!enif_get_local_pid(env, argv[2], &pid))
	  return make_error_tuple(env, "invalid_pid");

     /* Note, no check is made for the type of the argument */
     cmd = command_create();
     if(!cmd)
	  return make_error_tuple(env, "command_create_failed");

     /* command */
     cmd->type = cmd_open;
     cmd->ref = enif_make_copy(cmd->env, argv[1]);
     cmd->pid = pid;
     cmd->arg = enif_make_copy(cmd->env, argv[3]);

     if(!queue_push(db->commands, cmd))
	  return make_error_tuple(env, "command_push_failed");

     return make_atom(env, "ok");
}

/*
 * Execute the sql statement
 */
static ERL_NIF_TERM
esqlite_exec(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
     esqlite_connection *db;
     esqlite_command *cmd = NULL;
     ErlNifPid pid;

     if(argc != 4)
	  return enif_make_badarg(env);

     if(!enif_get_resource(env, argv[0], esqlite_connection_type, (void **) &db))
	  return enif_make_badarg(env);

     if(!enif_is_ref(env, argv[1]))
	  return make_error_tuple(env, "invalid_ref");

     if(!enif_get_local_pid(env, argv[2], &pid))
	  return make_error_tuple(env, "invalid_pid");

     cmd = command_create();
     if(!cmd)
	  return make_error_tuple(env, "command_create_failed");

     /* command */
     cmd->type = cmd_exec;
     cmd->ref = enif_make_copy(cmd->env, argv[1]);
     cmd->pid = pid;
     cmd->arg = enif_make_copy(cmd->env, argv[3]);

     if(!queue_push(db->commands, cmd))
	  return make_error_tuple(env, "command_push_failed");

     return make_atom(env, "ok");
}


/*
 * Prepare the sql statement
 */
static ERL_NIF_TERM
esqlite_prepare(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
     esqlite_connection *conn;
     esqlite_command *cmd = NULL;
     ErlNifPid pid;

     if(argc != 4)
	  return enif_make_badarg(env);
     if(!enif_get_resource(env, argv[0], esqlite_connection_type, (void **) &conn))
	  return enif_make_badarg(env);
     if(!enif_is_ref(env, argv[1]))
	  return make_error_tuple(env, "invalid_ref");
     if(!enif_get_local_pid(env, argv[2], &pid))
	  return make_error_tuple(env, "invalid_pid");

     cmd = command_create();
     if(!cmd)
	  return make_error_tuple(env, "command_create_failed");

     cmd->type = cmd_prepare;
     cmd->ref = enif_make_copy(cmd->env, argv[1]);
     cmd->pid = pid;
     cmd->arg = enif_make_copy(cmd->env, argv[3]);

     if(!queue_push(conn->commands, cmd))
	  return make_error_tuple(env, "command_push_failed");

     return make_atom(env, "ok");
}

/*
 * Bind a variable to a prepared statement
 */
static ERL_NIF_TERM
esqlite_bind(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
     esqlite_statement *stmt;
     esqlite_command *cmd = NULL;
     ErlNifPid pid;

     if(argc != 4)
	  return enif_make_badarg(env);
     if(!enif_get_resource(env, argv[0], esqlite_statement_type, (void **) &stmt))
	  return enif_make_badarg(env);
     if(!enif_is_ref(env, argv[1]))
	  return make_error_tuple(env, "invalid_ref");
     if(!enif_get_local_pid(env, argv[2], &pid))
	  return make_error_tuple(env, "invalid_pid");

     cmd = command_create();
     if(!cmd)
	  return make_error_tuple(env, "command_create_failed");

     cmd->type = cmd_bind;
     cmd->ref = enif_make_copy(cmd->env, argv[1]);
     cmd->pid = pid;
     cmd->stmt = stmt->statement;
     cmd->arg = enif_make_copy(cmd->env, argv[3]);

     if(!stmt->connection)
	  return make_error_tuple(env, "no_connection");
     if(!stmt->connection->commands)
	  return make_error_tuple(env, "no_command_queue");

     if(!queue_push(stmt->connection->commands, cmd))
	  return make_error_tuple(env, "command_push_failed");

     return make_atom(env, "ok");
}

/*
 * Step to a prepared statement
 */
static ERL_NIF_TERM
esqlite_step(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
     esqlite_statement *stmt;
     esqlite_command *cmd = NULL;
     ErlNifPid pid;

     if(argc != 3)
	  return enif_make_badarg(env);
     if(!enif_get_resource(env, argv[0], esqlite_statement_type, (void **) &stmt))
	  return enif_make_badarg(env);
     if(!enif_is_ref(env, argv[1]))
	  return make_error_tuple(env, "invalid_ref");
     if(!enif_get_local_pid(env, argv[2], &pid))
	  return make_error_tuple(env, "invalid_pid");

     if(!stmt->statement)
	  return make_error_tuple(env, "no_prepared_statement");

     cmd = command_create();
     if(!cmd)
	  return make_error_tuple(env, "command_create_failed");

     cmd->type = cmd_step;
     cmd->ref = enif_make_copy(cmd->env, argv[1]);
     cmd->pid = pid;
     cmd->stmt = stmt->statement;

     if(!stmt->connection)
	  return make_error_tuple(env, "no_connection");
     if(!stmt->connection->commands)
	  return make_error_tuple(env, "no_command_queue");

     if(!queue_push(stmt->connection->commands, cmd))
	  return make_error_tuple(env, "command_push_failed");

     return make_atom(env, "ok");
}

/*
 * Step to a prepared statement
 */
static ERL_NIF_TERM
esqlite_column_names(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
     esqlite_statement *stmt;
     esqlite_command *cmd = NULL;
     ErlNifPid pid;

     if(argc != 3)
	  return enif_make_badarg(env);
     if(!enif_get_resource(env, argv[0], esqlite_statement_type, (void **) &stmt))
	  return enif_make_badarg(env);
     if(!enif_is_ref(env, argv[1]))
	  return make_error_tuple(env, "invalid_ref");
     if(!enif_get_local_pid(env, argv[2], &pid))
	  return make_error_tuple(env, "invalid_pid");

     if(!stmt->statement)
	  return make_error_tuple(env, "no_prepared_statement");

     cmd = command_create();
     if(!cmd)
	  return make_error_tuple(env, "command_create_failed");

     cmd->type = cmd_column_names;
     cmd->ref = enif_make_copy(cmd->env, argv[1]);
     cmd->pid = pid;
     cmd->stmt = stmt->statement;

     if(!stmt->connection)
	  return make_error_tuple(env, "no_connection");
     if(!stmt->connection->commands)
	  return make_error_tuple(env, "no_command_queue");

     if(!queue_push(stmt->connection->commands, cmd))
	  return make_error_tuple(env, "command_push_failed");

     return make_atom(env, "ok");
}

/*
 * Close the database
 */
static ERL_NIF_TERM
esqlite_close(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
     esqlite_connection *conn;
     esqlite_command *cmd = NULL;
     ErlNifPid pid;

     if(!enif_get_resource(env, argv[0], esqlite_connection_type, (void **) &conn))
	  return enif_make_badarg(env);

     if(!enif_is_ref(env, argv[1]))
	  return make_error_tuple(env, "invalid_ref");

     if(!enif_get_local_pid(env, argv[2], &pid))
	  return make_error_tuple(env, "invalid_pid");

     cmd = command_create();
     if(!cmd)
	  return make_error_tuple(env, "command_create_failed");

     /* Push the close command on the queue
      */
     cmd->type = cmd_close;
     cmd->ref = enif_make_copy(cmd->env, argv[1]);
     cmd->pid = pid;
     if(!queue_push(conn->commands, cmd))
	  return make_error_tuple(env, "command_push_failed");

     return make_atom(env, "ok");
}

/*
 * Load the nif. Initialize some stuff and such
 */
static int
on_load(ErlNifEnv* env, void** priv, ERL_NIF_TERM info)
{
     ErlNifResourceType *rt;

     rt = enif_open_resource_type(env, "esqlite3_nif", "esqlite_connection_type",
				  destruct_esqlite_connection, ERL_NIF_RT_CREATE, NULL);
     if(!rt)
	  return -1;
     esqlite_connection_type = rt;

     rt =  enif_open_resource_type(env, "esqlite3_nif", "esqlite_statement_type",
				   destruct_esqlite_statement, ERL_NIF_RT_CREATE, NULL);
     if(!rt)
	  return -1;
     esqlite_statement_type = rt;

     return 0;
}

static ErlNifFunc nif_funcs[] = {
     {"start", 0, esqlite_start},
     {"open", 4, esqlite_open},
     {"exec", 4, esqlite_exec},
     {"prepare", 4, esqlite_prepare},
     {"step", 3, esqlite_step},
     // {"esqlite_bind", 3, esqlite_bind_named},
     {"bind", 4, esqlite_bind},
     {"column_names", 3, esqlite_column_names},
     {"close", 3, esqlite_close}
};

ERL_NIF_INIT(esqlite3_nif, nif_funcs, on_load, NULL, NULL, NULL);
