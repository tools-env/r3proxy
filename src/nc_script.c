#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <ctype.h>

#include <nc_core.h>

static int 
split(lua_State *L) 
{
    const char *string = luaL_checkstring(L, 1);
    const char *sep = luaL_checkstring(L, 2);
    const char *token;
    int i = 1;
    lua_newtable(L);
    while ((token = strchr(string, *sep)) != NULL) {
        lua_pushlstring(L, string, token - string);
        lua_rawseti(L, -2, i++);
        string = token + 1;
    }
    lua_pushstring(L, string);
    lua_rawseti(L, -2, i);
    return 1;
}
 
static int 
strip(lua_State *L) 
{
    const char *front;
    const char *end;
    size_t      size;
 
    front = luaL_checklstring(L, 1, &size);
    end   = &front[size - 1];

    for ( ; size && isspace(*front) ; size-- , front++)
        ;
    for ( ; size && isspace(*end) ; size-- , end--)
        ;
    
    lua_pushlstring(L, front, (size_t)(end - front) + 1);
    return 1;
}   
    
static const luaL_Reg stringext[] = {
    {"split", split},
    {"strip", strip},
    {NULL, NULL}
};

/* module replicaset */
static int 
replicaset_new(lua_State *L) {
    size_t i, nbytes = sizeof(struct replicaset);
    struct replicaset *rs = (struct replicaset*)lua_newuserdata(L, nbytes);
    if (rs == NULL) {
        log_error("failed to allocate memory");
    }
    for (i = 0; i < NC_MAXTAGNUM; i++) {
        array_init(&rs->tagged_servers[i], 2, sizeof(struct server *));
    }
    return 1;
}

static int
replicaset_set_master(lua_State *L) {
    struct replicaset *rs = (struct replicaset*)lua_touserdata(L, 1);
    struct server *master = (struct server*)lua_touserdata(L, 2);
    rs->master = master;
    return 0;
}

static int
replicaset_get_master(lua_State *L) {
    struct replicaset *rs = (struct replicaset*)lua_touserdata(L, 1);
    lua_pushlightuserdata(L, rs->master);
    return 1;
}

static int
replicaset_add_slave(lua_State *L) {
    struct replicaset *rs = (struct replicaset*)lua_touserdata(L, 1);
    int idx = lua_tonumber(L, 2);
    struct server *slave = (struct server*)lua_touserdata(L, 3);
    struct server **s = array_push(&rs->tagged_servers[idx]);
    *s = slave;
    return 0;
}

static const luaL_Reg replicaset_funcs[] = {
    {"new", replicaset_new},
    {"set_master", replicaset_set_master},
    {"get_master", replicaset_get_master},
    {"add_slave", replicaset_add_slave},
    {NULL, NULL}
};

static int 
luaopen_replicaset(lua_State* L) {
    luaL_newlib(L, replicaset_funcs);
    return 1;
}

/* module server */
static int 
server_new(lua_State *L) {
    struct server *s;
    struct server_pool *pool;
    const char* tmpstr;
    struct string address;
    rstatus_t status;

    s = (struct server*)lua_newuserdata(L, sizeof(struct server));
    if (s == NULL) {
        log_error("failed to allocate memory");
    }

    /* init server struct */
    /* set owner */
    lua_getglobal(L, "__pool");
    pool = (struct server_pool*)lua_touserdata(L, -1);
    s->owner = pool;
    lua_pop(L, 1);

    s->idx = 0;
    s->weight = 1;
    /* set name */
    tmpstr = lua_tostring(L, 1);
    string_init(&s->name);
    string_copy(&s->name, tmpstr, strlen(tmpstr));
    string_init(&s->pname);
    string_copy(&s->pname, tmpstr, strlen(tmpstr));
    /* set ip */
    tmpstr = lua_tostring(L, 2);
    string_init(&address);
    string_copy(&address, tmpstr, strlen(tmpstr));
    /* set port */
    s->port = (uint16_t)lua_tointeger(L, 3);

    status = nc_resolve(&address, s->port, &s->sockinfo);
    if (status != NC_OK) {
        log_error("conf: failed to resolve %.*s:%d", address.len, address.data, s->port);
        string_deinit(&address);
        lua_pop(L, 1);
        lua_pushnil(L);
        return 1;
    }

    s->family = s->sockinfo.family;
    s->addrlen = s->sockinfo.addrlen;
    s->addr = (struct sockaddr *)&s->sockinfo.addr;

    s->ns_conn_q = 0;
    TAILQ_INIT(&s->s_conn_q);

    s->next_retry = 0LL;
    s->failure_count = 0;

    return 1;
}

static int
server_preconnect(lua_State *L) {
    struct server *server;
    struct server_pool *pool;
    struct conn *conn;
    rstatus_t status;

    server = (struct server*)lua_touserdata(L, 1);
    pool = server->owner;
    conn = server_conn(server);
    if (conn == NULL) {
        lua_pushboolean(L, 0);
        return 1;
    }

    status = server_connect(pool->ctx, server, conn);
    if (status != NC_OK) {
        log_warn("script: connect to server '%.*s' failed, ignored: %s",
                 server->pname.len, server->pname.data, strerror(errno));
        server_close(pool->ctx, conn);
        lua_pushboolean(L, 0);
        return 1;
    }

    lua_pushboolean(L, 1);
    return 1;
}

static int
server_disconnect(lua_State *L) {
    struct server *server;
    struct server_pool *pool;

    server = (struct server*)lua_touserdata(L, 1);
    pool = server->owner;
    
    while (!TAILQ_EMPTY(&server->s_conn_q)) {
        struct conn *conn;

        ASSERT(server->ns_conn_q > 0);

        conn = TAILQ_FIRST(&server->s_conn_q);
        conn->close(pool->ctx, conn);
    }

    return 0;
}

static const luaL_Reg server_funcs[] = {
    {"new", server_new},
    {"connect", server_preconnect},
    {"disconnect", server_disconnect},
    {NULL, NULL}
};

static int 
luaopen_server(lua_State* L) {
    luaL_newlib(L, server_funcs);
    return 1;
}

/* module slots */

static int
slots_set_replicaset(lua_State *L) {
    struct server_pool *pool;
    struct replicaset *rs;
    int i, start, end;

    rs = (struct replicaset*)lua_touserdata(L, 1);
    start = (int)lua_tonumber(L, 2);
    end = (int)lua_tonumber(L, 3);

    lua_getglobal(L, "__pool");
    pool = (struct server_pool*)lua_touserdata(L, -1);

    log_debug(LOG_VERB, "update slots %d-%d", start, end);

    for (i = start; i <= end; i++) {
        pool->slots[i] = rs;
    }

    return 0;
}

static const luaL_Reg slots_funcs[] = {
    {"set_replicaset", slots_set_replicaset},
    {NULL, NULL}
};

static int 
luaopen_slots(lua_State* L) {
    luaL_newlib(L, slots_funcs);
    return 1;
}

/* module pool */

static int
pool_get_region(lua_State *L) {
    struct server_pool *pool;

    lua_getglobal(L, "__pool");
    pool = (struct server_pool*)lua_touserdata(L, -1);

    lua_pushlstring(L, (const char*)pool->region.data, pool->region.len);
    return 1;
}

static int
pool_get_avaliable_zone(lua_State *L) {
    struct server_pool *pool;

    lua_getglobal(L, "__pool");
    pool = (struct server_pool*)lua_touserdata(L, -1);

    lua_pushlstring(L, (const char*)pool->avaliable_zone.data, pool->avaliable_zone.len);
    return 1;
}

static int
pool_get_failover_zones(lua_State *L) {
    struct server_pool *pool;

    lua_getglobal(L, "__pool");
    pool = (struct server_pool*)lua_touserdata(L, -1);

    lua_pushlstring(L, (const char*)pool->failover_zones.data, pool->failover_zones.len);
    return 1;
}

static int
pool_get_machine_room(lua_State *L) {
    struct server_pool *pool;

    lua_getglobal(L, "__pool");
    pool = (struct server_pool*)lua_touserdata(L, -1);

    lua_pushlstring(L, (const char*)pool->machine_room.data, pool->machine_room.len);
    return 1;
}

static const luaL_Reg pool_funcs[] = {
    {"region", pool_get_region},
    {"avaliable_zone", pool_get_avaliable_zone},
    {"failover_zones", pool_get_failover_zones},
    {"machine_room", pool_get_machine_room},
    {NULL, NULL}
};

static int 
luaopen_pool(lua_State* L) {
    luaL_newlib(L, pool_funcs);
    return 1;
}

/* init */

int script_init(struct server_pool *pool)
{
    lua_State *L;

    L = luaL_newstate();                        /* Create Lua state variable */
    pool->L = L;
    luaL_openlibs(L);                           /* Load Lua libraries */
    if (luaL_loadfile(L, "test.lua")) {
        log_debug(LOG_VERB, "init lua script failed - %s", lua_tostring(L, -1));
        return 1;
    }

    lua_getglobal(L, "string");
    luaL_setfuncs(L, stringext, 0);
    lua_setglobal(L, "string");

    lua_pushlightuserdata(L, pool);
    lua_setglobal(L, "__pool");

    luaL_requiref(L, "replicaset", &luaopen_replicaset, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "server", &luaopen_server, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "slots", &luaopen_slots, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "pool", &luaopen_pool, 1);
    lua_pop(L, 1);

    if (lua_pcall(L, 0, 0, 0) != 0) {
        log_error("call lua script failed - %s", lua_tostring(L, -1));
    }

    return 0;
}

int script_call(struct server_pool *pool, const char *body, int len, const char *func_name)
{
    lua_State *L = pool->L;

    log_debug(LOG_VERB, "update cluster nodes");

    lua_getglobal(L, func_name);
    lua_pushlstring(L, body, len);

    /* call update function */
    if (lua_pcall(L, 1, 0, 0) != 0) {
        log_debug(LOG_VERB, "call %s failed - %s", func_name, lua_tostring(L, -1));
    }

    return 0;
}