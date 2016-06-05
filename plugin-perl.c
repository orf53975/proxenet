#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef _PERL_PLUGIN

/*******************************************************************************
 *
 * Perl plugin
 *
 */


#include <EXTERN.h>
#include <perl.h>
#include <string.h>
#include <stdlib.h>

#ifdef __LINUX__
#include <alloca.h>
#endif

#include "core.h"
#include "plugin.h"
#include "plugin-perl.h"
#include "utils.h"
#include "main.h"

#define xlog_perl(t, ...) xlog(t, "["_PERL_VERSION_"] " __VA_ARGS__)


static PerlInterpreter *my_perl;


/**
 * Allocates a buffer with the full function path in the path.
 * The buffer *must* be free-ed by the caller.
 */
static char* proxenet_perl_get_function_path(char *funcname)
{
        char *path;
        char *required  = NULL;
	char *package_name = NULL;
	size_t package_len, len = 0;
        SV* package_sv = NULL;

        /* Get the package name from the package (which should follow the convention...) */
        package_sv = get_sv("package", 0);
        if (!SvPOK(package_sv)) {
                xlog_perl(LOG_ERROR, "Invalid convention for '%s': the package should return a string\n", funcname);
                return NULL;
        }

        required = (char*) SvPV_nolen(package_sv);
        package_len = strlen(required);
        package_name = (char*) alloca(package_len+1);
        proxenet_xzero(package_name, package_len+1);
        memcpy(package_name, required, package_len);

        len = package_len + 2 + strlen(funcname) + 1;
        path = proxenet_xmalloc(len);
        proxenet_xsnprintf(path, len, "%s::%s", package_name, funcname);

#ifdef DEBUG
        xlog_perl(LOG_DEBUG, "Loaded '%s'\n", path);
#endif

        return path;
}


/**
 *
 */
int proxenet_perl_load_file(plugin_t* plugin)
{
	SV* sv = NULL;
	int nb_res = -1;
	int ret = 0;
        char *pathname;

        if(plugin->state != INACTIVE){
#ifdef DEBUG
                if(cfg->verbose > 2)
                        xlog_perl(LOG_DEBUG, "Plugin '%s' is already loaded. Skipping...\n", plugin->name);
#endif
                return 0;
        }

        pathname = plugin->fullpath;

#ifdef DEBUG
	xlog_perl(LOG_DEBUG, "Loading '%s'\n", pathname);
#endif

	/* Load the file through perl's require mechanism */
	dSP;
	ENTER;
	SAVETMPS;

	PUSHMARK(SP);
	PUTBACK;

	sv = newSVpvf("$package = require q%c%s%c", 0, pathname, 0);
	nb_res = eval_sv(sv_2mortal(sv), G_EVAL);

	if (nb_res != 1) {
		xlog_perl(LOG_ERROR,
		     "Invalid number of response returned while loading '%s' (got %d, expected 1)\n",
		     pathname,
		     nb_res);

	} else if (SvTRUE(ERRSV)) {
		xlog_perl(LOG_ERROR, "Eval error for '%s': %s\n", pathname, SvPV_nolen(ERRSV));

	} else {
                plugin->pre_function = proxenet_perl_get_function_path(CFG_REQUEST_PLUGIN_FUNCTION);
                if(!plugin->pre_function){
                        ret = -1;
                        goto perl_epilog;
                }

                plugin->post_function = proxenet_perl_get_function_path(CFG_RESPONSE_PLUGIN_FUNCTION);
                if(!plugin->post_function){
                        ret = -1;
                        goto perl_epilog;
                }

                plugin->onload_function = proxenet_perl_get_function_path(CFG_ONLOAD_PLUGIN_FUNCTION);
                plugin->onleave_function = proxenet_perl_get_function_path(CFG_ONLEAVE_PLUGIN_FUNCTION);

                if (cfg->verbose > 2)
                        xlog_perl(LOG_INFO, "Package '%s' loaded\n", plugin->name);

                if (plugin->onload_function){
                        call_pv(plugin->onload_function, G_EVAL);
                }

	}

perl_epilog:

	SPAGAIN;
	PUTBACK;
	FREETMPS;
	LEAVE;

	return ret;
}




/**
 *
 */
int proxenet_perl_initialize_vm(plugin_t* plugin)
{
	interpreter_t *interpreter;
        char *perl_args[] = { "", "/dev/null", NULL };
        int   perl_args_count = 2;

#ifdef PERL_SYS_INIT3
        int a;
        char **perl_args_local;
        char *perl_env[] = {};
        a = perl_args_count;
        perl_args_local = perl_args;
        (void) perl_env;
        PERL_SYS_INIT3 (&a, (char ***)&perl_args_local, (char ***)&perl_env);
#endif

	interpreter = plugin->interpreter;

	/* checks */
	if (interpreter->ready)
                return 0;

#ifdef DEBUG
        xlog_perl(LOG_DEBUG, "%s\n", "Initializing VM");
#endif

        /* vm init */
        my_perl = perl_alloc();
        perl_construct(my_perl);
        PL_exit_flags |= PERL_EXIT_DESTRUCT_END;

        if (!my_perl) {
                xlog_perl(LOG_ERROR, "%s\n", "failed init-ing vm");
                return -1;
        }

        perl_parse(my_perl, NULL, perl_args_count, perl_args, (char **)NULL);

        interpreter->vm = (void*) my_perl;
        interpreter->ready = true;

	return 0;
}


/**
 *
 */
int proxenet_perl_destroy_plugin(plugin_t* plugin)
{
        proxenet_plugin_set_state(plugin, INACTIVE);

        if (plugin->onleave_function){
                dSP;
                ENTER;
                SAVETMPS;
                PUSHMARK(SP);
                PUTBACK;

                call_pv(plugin->onleave_function, G_EVAL);

                SPAGAIN;
                PUTBACK;
                FREETMPS;
                LEAVE;
        }

        proxenet_xfree(plugin->pre_function);
        proxenet_xfree(plugin->post_function);
        if (plugin->onload_function)
                proxenet_xfree(plugin->onload_function);
        if (plugin->onleave_function)
                proxenet_xfree(plugin->onleave_function);
        return 0;
}


/**
 *
 */
int proxenet_perl_destroy_vm(interpreter_t* interpreter)
{
	perl_destruct(my_perl);
	perl_free(my_perl);
        my_perl = NULL;

        /* Looks like this macro segfaults on FreeBSD and OSX.... */
#if defined(PERL_SYS_TERM) && !defined(__FreeBSD__) && !defined(__DARWIN__)
        PERL_SYS_TERM ();
#endif

	interpreter->vm = NULL;
	interpreter->ready = false;
	return 0;
}


/**
 *
 */
static char* proxenet_perl_execute_function(char* fname, long rid, char* request_str, size_t* request_size, char* uri)
{
	char *res, *data;
	int nb_res;
	size_t len;
	SV* sv = NULL;

	data = NULL;

	dSP;
	ENTER;
	SAVETMPS;

	PUSHMARK(SP);
	XPUSHs(sv_2mortal(newSVuv(rid)));
	XPUSHs(sv_2mortal(newSVpvn(request_str, *request_size)));
        XPUSHs(sv_2mortal(newSVpvn(uri, strlen(uri))));
	PUTBACK;

	nb_res = call_pv(fname, G_EVAL | G_SCALAR);

	SPAGAIN;

	if (nb_res != 1) {
		xlog_perl(LOG_ERROR, "Unexpected number of response (got %d, expected 1)\n", nb_res);
	} else if (SvTRUE(ERRSV)) {
		xlog_perl(LOG_ERROR, "call_pv() error for '%s': %s\n", fname, SvPV_nolen(ERRSV));
        } else {
		sv = POPs;
		res = SvPV(sv, len);
		data = (char*) proxenet_xmalloc(len+1);
		memcpy(data, res, len);
		*request_size = len;
	}

	PUTBACK;
	FREETMPS;
	LEAVE;

	return data;
}


/**
 *
 */
static void proxenet_perl_lock_vm(interpreter_t *interpreter)
{
	pthread_mutex_lock(&interpreter->mutex);
}


/**
 *
 */
static void proxenet_perl_unlock_vm(interpreter_t *interpreter)
{
	pthread_mutex_unlock(&interpreter->mutex);
}


/**
 *
 */
char* proxenet_perl_plugin(plugin_t* plugin, request_t* request)
{
	interpreter_t *interpreter;
        char *function_name;
	char *buf, *uri;

	interpreter = plugin->interpreter;
	if (!interpreter->ready)
		return NULL;

        uri = request->http_infos.uri;

	if (request->type == REQUEST)
		function_name = plugin->pre_function;
	else
		function_name = plugin->post_function;

	proxenet_perl_lock_vm(interpreter);
	buf = proxenet_perl_execute_function(function_name,
                                             request->id,
                                             request->data,
                                             &request->size,
                                             uri);
	proxenet_perl_unlock_vm(interpreter);

	return buf;
}



#endif /* _PERL_PLUGIN */
