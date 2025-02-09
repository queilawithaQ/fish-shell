/** \file env.c
  Functions for setting and getting environment variables.
*/
#include "config.h"

#include <stdlib.h>
#include <wchar.h>
#include <string.h>
#include <stdio.h>
#include <locale.h>
#include <unistd.h>
#include <signal.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pthread.h>
#include <pwd.h>
#include <set>
#include <map>
#include <algorithm>

#if HAVE_NCURSES_H
#include <ncurses.h>
#else
#include <curses.h>
#endif

#if HAVE_TERM_H
#include <term.h>
#elif HAVE_NCURSES_TERM_H
#include <ncurses/term.h>
#endif

#if HAVE_LIBINTL_H
#include <libintl.h>
#endif

#include <errno.h>

#include "fallback.h"
#include "util.h"

#include "wutil.h"
#include "proc.h"
#include "common.h"
#include "env.h"
#include "sanity.h"
#include "expand.h"
#include "history.h"
#include "reader.h"
#include "parser.h"
#include "env_universal.h"
#include "input.h"
#include "event.h"
#include "path.h"

#include "complete.h"

/** Command used to start fishd */
#define FISHD_CMD L"fishd ^ $__fish_runtime_dir/fishd.log.%s"

// Version for easier debugging
//#define FISHD_CMD L"fishd"

/** Value denoting a null string */
#define ENV_NULL L"\x1d"

/** Some configuration path environment variables */
#define FISH_DATADIR_VAR L"__fish_datadir"
#define FISH_SYSCONFDIR_VAR L"__fish_sysconfdir"
#define FISH_HELPDIR_VAR L"__fish_help_dir"
#define FISH_BIN_DIR L"__fish_bin_dir"

/**
   At init, we read all the environment variables from this array.
*/
extern char **environ;
/**
   This should be the same thing as \c environ, but it is possible only one of the two work...
*/
extern char **__environ;

/**
   A variable entry. Stores the value of a variable and whether it
   should be exported. Obviously, it needs to be allocated large
   enough to fit the value string.
*/
struct var_entry_t
{
    wcstring val; /**< The value of the variable */
    bool exportv; /**< Whether the variable should be exported */

    var_entry_t() : exportv(false) { }
};

typedef std::map<wcstring, var_entry_t> var_table_t;

bool g_log_forks = false;
bool g_use_posix_spawn = false; //will usually be set to true


/**
   Struct representing one level in the function variable stack
*/
struct env_node_t
{
    /**
      Variable table
    */
    var_table_t env;
    /**
      Does this node imply a new variable scope? If yes, all
      non-global variables below this one in the stack are
      invisible. If new_scope is set for the global variable node,
      the universe will explode.
    */
    bool new_scope;
    /**
       Does this node contain any variables which are exported to subshells
    */
    bool exportv;

    /**
      Pointer to next level
    */
    struct env_node_t *next;


    env_node_t() : new_scope(false), exportv(false), next(NULL) { }

    /* Returns a pointer to the given entry if present, or NULL. */
    const var_entry_t *find_entry(const wcstring &key);

    /* Returns the next scope to search in order, respecting the new_scope flag, or NULL if we're done. */
    env_node_t *next_scope_to_search(void);
};

class variable_entry_t
{
    bool exportv; /**< Whether the variable should be exported */
    wcstring value; /**< Value of the variable */
};

static pthread_mutex_t env_lock = PTHREAD_MUTEX_INITIALIZER;

/** Top node on the function stack */
static env_node_t *top = NULL;

/** Bottom node on the function stack */
static env_node_t *global_env = NULL;


/**
   Table for global variables
*/
static var_table_t *global;

/* Helper class for storing constant strings, without needing to wrap them in a wcstring */

/* Comparer for const string set */
struct const_string_set_comparer
{
    bool operator()(const wchar_t *a, const wchar_t *b)
    {
        return wcscmp(a, b) < 0;
    }
};
typedef std::set<const wchar_t *, const_string_set_comparer> const_string_set_t;

/** Table of variables that may not be set using the set command. */
static const_string_set_t env_read_only;

static bool is_read_only(const wcstring &key)
{
    return env_read_only.find(key.c_str()) != env_read_only.end();
}

/**
   Table of variables whose value is dynamically calculated, such as umask, status, etc
*/
static const_string_set_t env_electric;

static bool is_electric(const wcstring &key)
{
    return env_electric.find(key.c_str()) != env_electric.end();
}

/**
   Exported variable array used by execv
*/
static null_terminated_array_t<char> export_array;

/**
   Flag for checking if we need to regenerate the exported variable
   array
*/
static bool has_changed_exported = true;
static void mark_changed_exported()
{
    has_changed_exported = true;
}

/**
   List of all locale variable names
*/
static const wchar_t * const locale_variable[] =
{
    L"LANG",
    L"LC_ALL",
    L"LC_COLLATE",
    L"LC_CTYPE",
    L"LC_MESSAGES",
    L"LC_MONETARY",
    L"LC_NUMERIC",
    L"LC_TIME",
    NULL
};


const var_entry_t *env_node_t::find_entry(const wcstring &key)
{
    const var_entry_t *result = NULL;
    var_table_t::const_iterator where = env.find(key);
    if (where != env.end())
    {
        result = &where->second;
    }
    return result;
}

env_node_t *env_node_t::next_scope_to_search(void)
{
    return this->new_scope ? global_env : this->next;
}


/**
   When fishd isn't started, this function is provided to
   env_universal as a callback, it tries to start up fishd. It's
   implementation is a bit of a hack, since it evaluates a bit of
   shellscript, and it might be used at times when that might not be
   the best idea.
*/
static void start_fishd()
{
    struct passwd *pw = getpwuid(getuid());

    debug(3, L"Spawning new copy of fishd");

    if (!pw)
    {
        debug(0, _(L"Could not get user information"));
        return;
    }

    wcstring cmd = format_string(FISHD_CMD, pw->pw_name);

    /* Prefer the fishd in __fish_bin_dir, if exists */
    const env_var_t bin_dir = env_get_string(L"__fish_bin_dir");
    if (! bin_dir.missing_or_empty())
    {
        wcstring path = bin_dir + L"/fishd";
        if (waccess(path, X_OK) == 0)
        {
            /* The path command just looks like 'fishd', so insert the bin path to make it absolute */
            cmd.insert(0, bin_dir + L"/");
        }
    }
    parser_t &parser = parser_t::principal_parser();
    parser.eval(cmd, io_chain_t(), TOP);
}

/**
   Return the current umask value.
*/
static mode_t get_umask()
{
    mode_t res;
    res = umask(0);
    umask(res);
    return res;
}

/** Checks if the specified variable is a locale variable */
static bool var_is_locale(const wcstring &key)
{
    for (size_t i=0; locale_variable[i]; i++)
    {
        if (key == locale_variable[i])
        {
            return true;
        }
    }
    return false;
}

/**
  Properly sets all locale information
*/
static void handle_locale()
{
    const env_var_t lc_all = env_get_string(L"LC_ALL");
    int i;
    const wcstring old_locale = wsetlocale(LC_MESSAGES, NULL);

    /*
      Array of locale constants corresponding to the local variable names defined in locale_variable
    */
    static const int cat[] =
    {
        0,
        LC_ALL,
        LC_COLLATE,
        LC_CTYPE,
        LC_MESSAGES,
        LC_MONETARY,
        LC_NUMERIC,
        LC_TIME
    }
    ;

    if (!lc_all.missing())
    {
        wsetlocale(LC_ALL, lc_all.c_str());
    }
    else
    {
        const env_var_t lang = env_get_string(L"LANG");
        if (!lang.missing())
        {
            wsetlocale(LC_ALL, lang.c_str());
        }

        for (i=2; locale_variable[i]; i++)
        {
            const env_var_t val = env_get_string(locale_variable[i]);

            if (!val.missing())
            {
                wsetlocale(cat[i], val.c_str());
            }
        }
    }

    const wcstring new_locale = wsetlocale(LC_MESSAGES, NULL);
    if (old_locale != new_locale)
    {

        /*
           Try to make change known to gettext. Both changing
           _nl_msg_cat_cntr and calling dcgettext might potentially
           tell some gettext implementation that the translation
           strings should be reloaded. We do both and hope for the
           best.
        */

        extern int _nl_msg_cat_cntr;
        _nl_msg_cat_cntr++;

        fish_dcgettext("fish", "Changing language to English", LC_MESSAGES);

        if (get_is_interactive())
        {
            debug(0, _(L"Changing language to English"));
        }
    }
}


/** React to modifying hte given variable */
static void react_to_variable_change(const wcstring &key)
{
    if (var_is_locale(key))
    {
        handle_locale();
    }
    else if (key == L"fish_term256")
    {
        update_fish_term256();
        reader_react_to_color_change();
    }
    else if (string_prefixes_string(L"fish_color_", key))
    {
        reader_react_to_color_change();
    }
}

/**
   Universal variable callback function. This function makes sure the
   proper events are triggered when an event occurs.
*/
static void universal_callback(fish_message_type_t type,
                               const wchar_t *name,
                               const wchar_t *val)
{
    const wchar_t *str = NULL;

    switch (type)
    {
        case SET:
        case SET_EXPORT:
        {
            str=L"SET";
            break;
        }

        case ERASE:
        {
            str=L"ERASE";
            break;
        }

        default:
            break;
    }

    if (str)
    {
        mark_changed_exported();

        event_t ev = event_t::variable_event(name);
        ev.arguments.push_back(L"VARIABLE");
        ev.arguments.push_back(str);
        ev.arguments.push_back(name);
        event_fire(&ev);
    }

    if (name)
        react_to_variable_change(name);
}

/**
   Make sure the PATH variable contains the essential directories
*/
static void setup_path()
{
    const wchar_t *path_el[] =
    {
        L"/bin",
        L"/usr/bin",
        NULL
    };

    env_var_t path = env_get_string(L"PATH");

    wcstring_list_t lst;
    if (! path.missing())
    {
        tokenize_variable_array(path, lst);
    }

    for (size_t j=0; path_el[j] != NULL; j++)
    {

        bool has_el = false;

        for (size_t i=0; i<lst.size(); i++)
        {
            const wcstring &el = lst.at(i);
            size_t len = el.size();

            while ((len > 0) && (el.at(len-1)==L'/'))
            {
                len--;
            }

            if ((wcslen(path_el[j]) == len) &&
                    (wcsncmp(el.c_str(), path_el[j], len)==0))
            {
                has_el = true;
                break;
            }
        }

        if (! has_el)
        {
            wcstring buffer;

            debug(3, L"directory %ls was missing", path_el[j]);

            if (!path.missing())
            {
                buffer += path;
            }

            buffer.append(ARRAY_SEP_STR);
            buffer.append(path_el[j]);

            env_set(L"PATH", buffer.empty()?NULL:buffer.c_str(), ENV_GLOBAL | ENV_EXPORT);

            path = env_get_string(L"PATH");
            lst.resize(0);
            tokenize_variable_array(path, lst);
        }
    }
}

int env_set_pwd()
{
    wchar_t dir_path[4096];
    wchar_t *res = wgetcwd(dir_path, 4096);
    if (!res)
    {
        return 0;
    }
    env_set(L"PWD", dir_path, ENV_EXPORT | ENV_GLOBAL);
    return 1;
}

wcstring env_get_pwd_slash(void)
{
    env_var_t pwd = env_get_string(L"PWD");
    if (pwd.missing_or_empty())
    {
        return L"";
    }
    if (! string_suffixes_string(L"/", pwd))
    {
        pwd.push_back(L'/');
    }
    return pwd;
}

/**
   Set up default values for various variables if not defined.
 */
static void env_set_defaults()
{

    if (env_get_string(L"USER").missing())
    {
        struct passwd *pw = getpwuid(getuid());
        if (pw->pw_name != NULL)
        {
            const wcstring wide_name = str2wcstring(pw->pw_name);
            env_set(L"USER", NULL, ENV_GLOBAL);
        }
    }

    if (env_get_string(L"HOME").missing())
    {
        const env_var_t unam = env_get_string(L"USER");
        char *unam_narrow = wcs2str(unam.c_str());
        struct passwd *pw = getpwnam(unam_narrow);
        if (pw->pw_dir != NULL)
        {
            const wcstring dir = str2wcstring(pw->pw_dir);
            env_set(L"HOME", dir.c_str(), ENV_GLOBAL);
        }
        free(unam_narrow);
    }

    env_set_pwd();

}

// Some variables should not be arrays. This used to be handled by a startup script, but we'd like to get down to 0 forks for startup, so handle it here.
static bool variable_can_be_array(const wcstring &key)
{
    if (key == L"DISPLAY")
    {
        return false;
    }
    else
    {
        return true;
    }
}

void env_init(const struct config_paths_t *paths /* or NULL */)
{
    /*
      env_read_only variables can not be altered directly by the user
    */

    const wchar_t * const ro_keys[] =
    {
        L"status",
        L"history",
        L"version",
        L"_",
        L"LINES",
        L"COLUMNS",
        L"PWD",
        L"SHLVL",
        L"FISH_VERSION",
    };
    for (size_t i=0; i < sizeof ro_keys / sizeof *ro_keys; i++)
    {
        env_read_only.insert(ro_keys[i]);
    }

    /*
      HOME and USER should be writeable by root, since this can be a
      convenient way to install software.
    */
    if (getuid() != 0)
    {
        env_read_only.insert(L"HOME");
        env_read_only.insert(L"USER");
    }

    /*
       Names of all dynamically calculated variables
       */
    env_electric.insert(L"history");
    env_electric.insert(L"status");
    env_electric.insert(L"umask");

    top = new env_node_t;
    global_env = top;
    global = &top->env;

    /*
      Now the environemnt variable handling is set up, the next step
      is to insert valid data
    */

    /*
      Import environment variables
    */
    for (char **p = (environ ? environ : __environ); p && *p; p++)
    {
        const wcstring key_and_val = str2wcstring(*p); //like foo=bar
        size_t eql = key_and_val.find(L'=');
        if (eql == wcstring::npos)
        {
            // no equals found
            env_set(key_and_val, L"", ENV_EXPORT);
        }
        else
        {
            wcstring key = key_and_val.substr(0, eql);
            wcstring val = key_and_val.substr(eql + 1);
            if (variable_can_be_array(val))
            {
                std::replace(val.begin(), val.end(), L':', ARRAY_SEP);
            }

            env_set(key, val.c_str(), ENV_EXPORT | ENV_GLOBAL);
        }
    }

    /* Set the given paths in the environment, if we have any */
    if (paths != NULL)
    {
        env_set(FISH_DATADIR_VAR, paths->data.c_str(), ENV_GLOBAL | ENV_EXPORT);
        env_set(FISH_SYSCONFDIR_VAR, paths->sysconf.c_str(), ENV_GLOBAL | ENV_EXPORT);
        env_set(FISH_HELPDIR_VAR, paths->doc.c_str(), ENV_GLOBAL | ENV_EXPORT);
        env_set(FISH_BIN_DIR, paths->bin.c_str(), ENV_GLOBAL | ENV_EXPORT);
    }

    /*
      Set up the PATH variable
    */
    setup_path();

    /*
      Set up the USER variable
    */
    const struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_name)
    {
        const wcstring uname = str2wcstring(pw->pw_name);
        env_set(L"USER", uname.c_str(), ENV_GLOBAL | ENV_EXPORT);
    }

    /*
      Set up the version variables
    */
    wcstring version = str2wcstring(PACKAGE_VERSION);
    env_set(L"version", version.c_str(), ENV_GLOBAL);
    env_set(L"FISH_VERSION", version.c_str(), ENV_GLOBAL);

    const env_var_t user_dir_wstr = env_get_string(L"USER");

    std::string fishd_dir = common_get_runtime_path();
    env_set(L"__fish_runtime_dir", str2wcstring(fishd_dir).c_str(), ENV_GLOBAL | ENV_EXPORT);

    wchar_t * user_dir = user_dir_wstr.missing()?NULL:const_cast<wchar_t*>(user_dir_wstr.c_str());

    env_universal_init(fishd_dir , user_dir ,
                       &start_fishd,
                       &universal_callback);

    /*
      Set up SHLVL variable
    */
    const env_var_t shlvl_str = env_get_string(L"SHLVL");
    wcstring nshlvl_str = L"1";
    if (! shlvl_str.missing())
    {
        long shlvl_i = wcstol(shlvl_str.c_str(), NULL, 10);
        if (shlvl_i >= 0)
        {
            nshlvl_str = to_string<long>(shlvl_i + 1);
        }
    }
    env_set(L"SHLVL", nshlvl_str.c_str(), ENV_GLOBAL | ENV_EXPORT);

    /* Set correct defaults for e.g. USER and HOME variables */
    env_set_defaults();

    /* Set g_log_forks */
    env_var_t log_forks = env_get_string(L"fish_log_forks");
    g_log_forks = ! log_forks.missing_or_empty() && from_string<bool>(log_forks);

    /* Set g_use_posix_spawn. Default to true. */
    env_var_t use_posix_spawn = env_get_string(L"fish_use_posix_spawn");
    g_use_posix_spawn = (use_posix_spawn.missing_or_empty() ? true : from_string<bool>(use_posix_spawn));
}

void env_destroy()
{
    env_universal_destroy();

    while (&top->env != global)
    {
        env_pop();
    }

    env_read_only.clear();
    env_electric.clear();

    var_table_t::iterator iter;
    for (iter = global->begin(); iter != global->end(); ++iter)
    {
        const var_entry_t &entry = iter->second;
        if (entry.exportv)
        {
            mark_changed_exported();
            break;
        }
    }

    delete top;
}

/**
   Search all visible scopes in order for the specified key. Return
   the first scope in which it was found.
*/
static env_node_t *env_get_node(const wcstring &key)
{
    env_node_t *env = top;
    while (env != NULL)
    {
        if (env->find_entry(key) != NULL)
        {
            break;
        }

        env = env->next_scope_to_search();
    }
    return env;
}

int env_set(const wcstring &key, const wchar_t *val, int var_mode)
{
    ASSERT_IS_MAIN_THREAD();
    bool has_changed_old = has_changed_exported;
    bool has_changed_new = false;
    int done=0;

    int is_universal = 0;

    if (val && contains(key, L"PWD", L"HOME"))
    {
        /* Canoncalize our path; if it changes, recurse and try again. */
        wcstring val_canonical = val;
        path_make_canonical(val_canonical);
        if (val != val_canonical)
        {
            return env_set(key, val_canonical.c_str(), var_mode);
        }
    }

    if ((var_mode & ENV_USER) && is_read_only(key))
    {
        return ENV_PERM;
    }

    if (key == L"umask")
    {
        wchar_t *end;

        /*
         Set the new umask
         */
        if (val && wcslen(val))
        {
            errno=0;
            long mask = wcstol(val, &end, 8);

            if (!errno && (!*end) && (mask <= 0777) && (mask >= 0))
            {
                umask(mask);
            }
        }
        /* Do not actually create a umask variable, on env_get, it will be calculated dynamically */
        return 0;
    }

    /*
     Zero element arrays are internaly not coded as null but as this
     placeholder string
     */
    if (!val)
    {
        val = ENV_NULL;
    }

    if (var_mode & ENV_UNIVERSAL)
    {
        bool exportv;
        if (var_mode & ENV_EXPORT)
        {
            // export
            exportv = true;
        }
        else if (var_mode & ENV_UNEXPORT)
        {
            // unexport
            exportv = false;
        }
        else
        {
            // not changing the export
            exportv = env_universal_get_export(key);
        }
        env_universal_set(key, val, exportv);
        is_universal = 1;

    }
    else
    {
        // Determine the node

        env_node_t *preexisting_node = env_get_node(key);
        bool preexisting_entry_exportv = false;
        if (preexisting_node != NULL)
        {
            var_table_t::const_iterator result = preexisting_node->env.find(key);
            assert(result != preexisting_node->env.end());
            const var_entry_t &entry = result->second;
            if (entry.exportv)
            {
                preexisting_entry_exportv = true;
                has_changed_new = true;
            }
        }

        env_node_t *node = NULL;
        if (var_mode & ENV_GLOBAL)
        {
            node = global_env;
        }
        else if (var_mode & ENV_LOCAL)
        {
            node = top;
        }
        else if (preexisting_node != NULL)
        {
            node = preexisting_node;

            if ((var_mode & (ENV_EXPORT | ENV_UNEXPORT)) == 0)
            {
                // use existing entry's exportv
                var_mode = preexisting_entry_exportv ? ENV_EXPORT : 0;
            }
        }
        else
        {
            if (! get_proc_had_barrier())
            {
                set_proc_had_barrier(true);
                env_universal_barrier();
            }

            if (env_universal_get(key))
            {
                bool exportv;
                if (var_mode & ENV_EXPORT)
                {
                    exportv = true;
                }
                else if (var_mode & ENV_UNEXPORT)
                {
                    exportv = false;
                }
                else
                {
                    exportv = env_universal_get_export(key);
                }

                env_universal_set(key, val, exportv);
                is_universal = 1;

                done = 1;

            }
            else
            {
                /*
                 New variable with unspecified scope. The default
                 scope is the innermost scope that is shadowing,
                 which will be either the current function or the
                 global scope.
                 */
                node = top;
                while (node->next && !node->new_scope)
                {
                    node = node->next;
                }
            }
        }

        if (!done)
        {
            // Set the entry in the node
            // Note that operator[] accesses the existing entry, or creates a new one
            var_entry_t &entry = node->env[key];
            if (entry.exportv)
            {
                // this variable already existed, and was exported
                has_changed_new = true;
            }
            entry.val = val;
            if (var_mode & ENV_EXPORT)
            {
                // the new variable is exported
                entry.exportv = true;
                node->exportv = true;
                has_changed_new = true;
            }
            else
            {
                entry.exportv = false;
            }

            if (has_changed_old || has_changed_new)
                mark_changed_exported();
        }

    }

    if (!is_universal)
    {
        event_t ev = event_t::variable_event(key);
        ev.arguments.push_back(L"VARIABLE");
        ev.arguments.push_back(L"SET");
        ev.arguments.push_back(key);

        //  debug( 1, L"env_set: fire events on variable %ls", key );
        event_fire(&ev);
        //  debug( 1, L"env_set: return from event firing" );
    }

    react_to_variable_change(key);

    return 0;
}


/**
   Attempt to remove/free the specified key/value pair from the
   specified map.

   \return zero if the variable was not found, non-zero otherwise
*/
static bool try_remove(env_node_t *n, const wchar_t *key, int var_mode)
{
    if (n == NULL)
    {
        return false;
    }

    var_table_t::iterator result = n->env.find(key);
    if (result != n->env.end())
    {
        if (result->second.exportv)
        {
            mark_changed_exported();
        }
        n->env.erase(result);
        return true;
    }

    if (var_mode & ENV_LOCAL)
    {
        return false;
    }

    if (n->new_scope)
    {
        return try_remove(global_env, key, var_mode);
    }
    else
    {
        return try_remove(n->next, key, var_mode);
    }
}


int env_remove(const wcstring &key, int var_mode)
{
    ASSERT_IS_MAIN_THREAD();
    env_node_t *first_node;
    int erased = 0;

    if ((var_mode & ENV_USER) && is_read_only(key))
    {
        return 2;
    }

    first_node = top;

    if (!(var_mode & ENV_UNIVERSAL))
    {

        if (var_mode & ENV_GLOBAL)
        {
            first_node = global_env;
        }

        if (try_remove(first_node, key.c_str(), var_mode))
        {
            event_t ev = event_t::variable_event(key);
            ev.arguments.push_back(L"VARIABLE");
            ev.arguments.push_back(L"ERASE");
            ev.arguments.push_back(key);

            event_fire(&ev);

            erased = 1;
        }
    }

    if (!erased &&
            !(var_mode & ENV_GLOBAL) &&
            !(var_mode & ENV_LOCAL))
    {
        erased = ! env_universal_remove(key.c_str());
    }

    react_to_variable_change(key);

    return !erased;
}

const wchar_t *env_var_t::c_str(void) const
{
    assert(! is_missing);
    return wcstring::c_str();
}

env_var_t env_get_string(const wcstring &key)
{
    /* Big hack...we only allow getting the history on the main thread. Note that history_t may ask for an environment variable, so don't take the lock here (we don't need it) */
    const bool is_main = is_main_thread();
    if (key == L"history" && is_main)
    {
        env_var_t result;

        history_t *history = reader_get_history();
        if (! history)
        {
            history = &history_t::history_with_name(L"fish");
        }
        if (history)
            history->get_string_representation(result, ARRAY_SEP_STR);
        return result;
    }
    else if (key == L"COLUMNS")
    {
        return to_string(common_get_width());
    }
    else if (key == L"LINES")
    {
        return to_string(common_get_width());
    }
    else if (key == L"status")
    {
        return to_string(proc_get_last_status());
    }
    else if (key == L"umask")
    {
        return format_string(L"0%0.3o", get_umask());
    }
    else
    {
        {
            /* Lock around a local region */
            scoped_lock lock(env_lock);

            env_node_t *env = top;
            wcstring result;

            while (env != NULL)
            {
                const var_entry_t *entry = env->find_entry(key);
                if (entry != NULL)
                {
                    if (entry->val == ENV_NULL)
                    {
                        return env_var_t::missing_var();
                    }
                    else
                    {
                        return entry->val;
                    }
                }

                env = env->next_scope_to_search();
            }
        }

        /* Another big hack - only do a universal barrier on the main thread (since it can change variable values)
           Make sure we do this outside the env_lock because it may itself call env_get_string */
        if (is_main && ! get_proc_had_barrier())
        {
            set_proc_had_barrier(true);
            env_universal_barrier();
        }

        const wchar_t *item = env_universal_get(key);

        if (!item || (wcscmp(item, ENV_NULL)==0))
        {
            return env_var_t::missing_var();
        }
        else
        {
            return item;
        }
    }
}

bool env_exist(const wchar_t *key, int mode)
{
    env_node_t *env;
    const wchar_t *item = NULL;

    CHECK(key, false);

    /*
      Read only variables all exist, and they are all global. A local
      version can not exist.
    */
    if (!(mode & ENV_LOCAL) && !(mode & ENV_UNIVERSAL))
    {
        if (is_read_only(key) || is_electric(key))
        {
            //Such variables are never exported
            if (mode & ENV_EXPORT)
            {
                return false;
            }
            else if (mode & ENV_UNEXPORT)
            {
                return true;
            }
            return true;
        }
    }

    if (!(mode & ENV_UNIVERSAL))
    {
        env = (mode & ENV_GLOBAL)?global_env:top;

        while (env != 0)
        {
            var_table_t::iterator result = env->env.find(key);

            if (result != env->env.end())
            {
                const var_entry_t &res = result->second;

                if (mode & ENV_EXPORT)
                {
                    return res.exportv;
                }
                else if (mode & ENV_UNEXPORT)
                {
                    return ! res.exportv;
                }

                return true;
            }

            if (mode & ENV_LOCAL)
                break;

            env = env->next_scope_to_search();
        }
    }

    if (!(mode & ENV_LOCAL) && !(mode & ENV_GLOBAL))
    {
        if (! get_proc_had_barrier())
        {
            set_proc_had_barrier(true);
            env_universal_barrier();
        }

        item = env_universal_get(key);

        if (item != NULL)
        {
            if (mode & ENV_EXPORT)
            {
                return env_universal_get_export(key) == 1;
            }
            else if (mode & ENV_UNEXPORT)
            {
                return env_universal_get_export(key) == 0;
            }

            return 1;
        }
    }

    return 0;
}

/**
   Returns true if the specified scope or any non-shadowed non-global subscopes contain an exported variable.
*/
static int local_scope_exports(env_node_t *n)
{

    if (n==global_env)
        return 0;

    if (n->exportv)
        return 1;

    if (n->new_scope)
        return 0;

    return local_scope_exports(n->next);
}

void env_push(bool new_scope)
{
    env_node_t *node = new env_node_t;
    node->next = top;
    node->new_scope=new_scope;

    if (new_scope)
    {
        if (local_scope_exports(top))
            mark_changed_exported();
    }
    top = node;

}


void env_pop()
{
    if (&top->env != global)
    {
        int i;
        int locale_changed = 0;

        env_node_t *killme = top;

        for (i=0; locale_variable[i]; i++)
        {
            var_table_t::iterator result =  killme->env.find(locale_variable[i]);
            if (result != killme->env.end())
            {
                locale_changed = 1;
                break;
            }
        }

        if (killme->new_scope)
        {
            if (killme->exportv || local_scope_exports(killme->next))
                mark_changed_exported();
        }

        top = top->next;

        var_table_t::iterator iter;
        for (iter = killme->env.begin(); iter != killme->env.end(); ++iter)
        {
            const var_entry_t &entry = iter->second;
            if (entry.exportv)
            {
                mark_changed_exported();
                break;
            }
        }

        delete killme;

        if (locale_changed)
            handle_locale();

    }
    else
    {
        debug(0,
              _(L"Tried to pop empty environment stack."));
        sanity_lose();
    }
}

/**
   Function used with to insert keys of one table into a set::set<wcstring>
*/
static void add_key_to_string_set(const var_table_t &envs, std::set<wcstring> *str_set, bool show_exported, bool show_unexported)
{
    var_table_t::const_iterator iter;
    for (iter = envs.begin(); iter != envs.end(); ++iter)
    {
        const var_entry_t &e = iter->second;

        if ((e.exportv && show_exported) ||
                (!e.exportv && show_unexported))
        {
            /* Insert this key */
            str_set->insert(iter->first);
        }

    }
}

wcstring_list_t env_get_names(int flags)
{
    scoped_lock lock(env_lock);

    wcstring_list_t result;
    std::set<wcstring> names;
    int show_local = flags & ENV_LOCAL;
    int show_global = flags & ENV_GLOBAL;
    int show_universal = flags & ENV_UNIVERSAL;

    env_node_t *n=top;
    const bool show_exported = (flags & ENV_EXPORT) || !(flags & ENV_UNEXPORT);
    const bool show_unexported = (flags & ENV_UNEXPORT) || !(flags & ENV_EXPORT);

    if (!show_local && !show_global && !show_universal)
    {
        show_local =show_universal = show_global=1;
    }

    if (show_local)
    {
        while (n)
        {
            if (n == global_env)
                break;

            add_key_to_string_set(n->env, &names, show_exported, show_unexported);
            if (n->new_scope)
                break;
            else
                n = n->next;

        }
    }

    if (show_global)
    {
        add_key_to_string_set(global_env->env, &names, show_exported, show_unexported);
        if (show_unexported)
        {
            result.insert(result.end(), env_electric.begin(), env_electric.end());
        }

        if (show_exported)
        {
            result.push_back(L"COLUMNS");
            result.push_back(L"LINES");
        }

    }

    if (show_universal)
    {

        wcstring_list_t uni_list;
        env_universal_get_names(uni_list,
                                show_exported,
                                show_unexported);
        names.insert(uni_list.begin(), uni_list.end());
    }

    result.insert(result.end(), names.begin(), names.end());
    return result;
}

/**
  Get list of all exported variables
*/

static void get_exported(const env_node_t *n, std::map<wcstring, wcstring> &h)
{
    if (!n)
        return;

    if (n->new_scope)
        get_exported(global_env, h);
    else
        get_exported(n->next, h);

    var_table_t::const_iterator iter;
    for (iter = n->env.begin(); iter != n->env.end(); ++iter)
    {
        const wcstring &key = iter->first;
        const var_entry_t &val_entry = iter->second;
        if (val_entry.exportv && (val_entry.val != ENV_NULL))
        {
            // Don't use std::map::insert here, since we need to overwrite existing values from previous scopes
            h[key] = val_entry.val;
        }
    }
}

static void export_func(const std::map<wcstring, wcstring> &envs, std::vector<std::string> &out)
{
    std::map<wcstring, wcstring>::const_iterator iter;
    for (iter = envs.begin(); iter != envs.end(); ++iter)
    {
        const std::string ks = wcs2string(iter->first);
        std::string vs = wcs2string(iter->second);

        for (size_t i=0; i < vs.size(); i++)
        {
            char &vc = vs.at(i);
            if (vc == ARRAY_SEP)
                vc = ':';
        }

        /* Put a string on the vector */
        out.push_back(std::string());
        std::string &str = out.back();
        str.reserve(ks.size() + 1 + vs.size());

        /* Append our environment variable data to it */
        str.append(ks);
        str.append("=");
        str.append(vs);
    }
}

static void update_export_array_if_necessary(bool recalc)
{
    ASSERT_IS_MAIN_THREAD();
    if (recalc && ! get_proc_had_barrier())
    {
        set_proc_had_barrier(true);
        env_universal_barrier();
    }

    if (has_changed_exported)
    {
        std::map<wcstring, wcstring> vals;
        size_t i;

        debug(4, L"env_export_arr() recalc");

        get_exported(top, vals);

        wcstring_list_t uni;
        env_universal_get_names(uni, 1, 0);
        for (i=0; i<uni.size(); i++)
        {
            const wcstring &key = uni.at(i);
            const wchar_t *val = env_universal_get(key);

            if (wcscmp(val, ENV_NULL))
            {
                // Note that std::map::insert does NOT overwrite a value already in the map,
                // which we depend on here
                vals.insert(std::pair<wcstring, wcstring>(key, val));
            }
        }

        std::vector<std::string> local_export_buffer;
        export_func(vals, local_export_buffer);
        export_array.set(local_export_buffer);
        has_changed_exported=false;
    }

}

const char * const *env_export_arr(bool recalc)
{
    ASSERT_IS_MAIN_THREAD();
    update_export_array_if_necessary(recalc);
    return export_array.get();
}

env_vars_snapshot_t::env_vars_snapshot_t(const wchar_t * const *keys)
{
    ASSERT_IS_MAIN_THREAD();
    wcstring key;
    for (size_t i=0; keys[i]; i++)
    {
        key.assign(keys[i]);
        const env_var_t val = env_get_string(key);
        if (! val.missing())
        {
            vars[key] = val;
        }
    }
}

env_vars_snapshot_t::env_vars_snapshot_t() { }

/* The "current" variables are not a snapshot at all, but instead trampoline to env_get_string, etc. We identify the current snapshot based on pointer values. */
static const env_vars_snapshot_t sCurrentSnapshot;
const env_vars_snapshot_t &env_vars_snapshot_t::current()
{
    return sCurrentSnapshot;
}

bool env_vars_snapshot_t::is_current() const
{
    return this == &sCurrentSnapshot;
}

env_var_t env_vars_snapshot_t::get(const wcstring &key) const
{
    /* If we represent the current state, bounce to env_get_string */
    if (this->is_current())
    {
        return env_get_string(key);
    }
    else
    {
        std::map<wcstring, wcstring>::const_iterator iter = vars.find(key);
        return (iter == vars.end() ? env_var_t::missing_var() : env_var_t(iter->second));
    }
}

const wchar_t * const env_vars_snapshot_t::highlighting_keys[] = {L"PATH", L"CDPATH", L"fish_function_path", NULL};
