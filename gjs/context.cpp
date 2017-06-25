/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil; -*- */
/*
 * Copyright (c) 2008  litl, LLC
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <config.h>

#include <array>

#include <gio/gio.h>

#include "context-private.h"
#include "global.h"
#include "importer.h"
#include "jsapi-private.h"
#include "jsapi-util.h"
#include "jsapi-wrapper.h"
#include "native.h"
#include "byteArray.h"
#include "runtime.h"
#include "gi/object.h"
#include "gi/repo.h"

#include <modules/modules.h>

#include <util/log.h>
#include <util/glib.h>
#include <util/error.h>

#ifdef G_OS_WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <string.h>

static void     gjs_context_dispose           (GObject               *object);
static void     gjs_context_finalize          (GObject               *object);
static void     gjs_context_constructed       (GObject               *object);
static void     gjs_context_get_property      (GObject               *object,
                                                  guint                  prop_id,
                                                  GValue                *value,
                                                  GParamSpec            *pspec);
static void     gjs_context_set_property      (GObject               *object,
                                                  guint                  prop_id,
                                                  const GValue          *value,
                                                  GParamSpec            *pspec);
struct _GjsContext {
    GObject parent;

    JSRuntime *runtime;
    JSContext *context;
    JS::Heap<JSObject*> global;
    GThread *owner_thread;

    char *program_name;

    char **search_path;

    bool destroying;

    bool should_exit;
    uint8_t exit_code;

    guint    auto_gc_id;

    std::array<JS::PersistentRootedId*, GJS_STRING_LAST> const_strings;
};

/* Keep this consistent with GjsConstString */
static const char *const_strings[] = {
    "constructor", "prototype", "length",
    "imports", "__parentModule__", "__init__", "searchPath",
    "__gjsKeepAlive", "__gjsPrivateNS",
    "gi", "versions", "overrides",
    "_init", "_instance_init", "_new_internal", "new",
    "message", "code", "stack", "fileName", "lineNumber", "name",
    "x", "y", "width", "height", "__modulePath__"
};

G_STATIC_ASSERT(G_N_ELEMENTS(const_strings) == GJS_STRING_LAST);

struct _GjsContextClass {
    GObjectClass parent;
};

G_DEFINE_TYPE(GjsContext, gjs_context, G_TYPE_OBJECT);

enum {
    PROP_0,
    PROP_SEARCH_PATH,
    PROP_PROGRAM_NAME,
};

static GMutex contexts_lock;
static GList *all_contexts = NULL;

static void
on_garbage_collect(JSRuntime *rt,
                   JSGCStatus status,
                   void      *data)
{
    /* We finalize any pending toggle refs before doing any garbage collection,
     * so that we can collect the JS wrapper objects, and in order to minimize
     * the chances of objects having a pending toggle up queued when they are
     * garbage collected. */
    if (status == JSGC_BEGIN)
        gjs_object_clear_toggles();
}

static void
gjs_context_init(GjsContext *js_context)
{
    gjs_context_make_current(js_context);
}

static void
gjs_context_class_init(GjsContextClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    GParamSpec *pspec;

    object_class->dispose = gjs_context_dispose;
    object_class->finalize = gjs_context_finalize;

    object_class->constructed = gjs_context_constructed;
    object_class->get_property = gjs_context_get_property;
    object_class->set_property = gjs_context_set_property;

    pspec = g_param_spec_boxed("search-path",
                               "Search path",
                               "Path where modules to import should reside",
                               G_TYPE_STRV,
                               (GParamFlags) (G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY));

    g_object_class_install_property(object_class,
                                    PROP_SEARCH_PATH,
                                    pspec);
    g_param_spec_unref(pspec);

    pspec = g_param_spec_string("program-name",
                                "Program Name",
                                "The filename of the launched JS program",
                                "",
                                (GParamFlags) (G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

    g_object_class_install_property(object_class,
                                    PROP_PROGRAM_NAME,
                                    pspec);
    g_param_spec_unref(pspec);

    /* For GjsPrivate */
    {
#ifdef G_OS_WIN32
        extern HMODULE gjs_dll;
        char *basedir = g_win32_get_package_installation_directory_of_module (gjs_dll);
        char *priv_typelib_dir = g_build_filename (basedir, "lib", "girepository-1.0", NULL);
        g_free (basedir);
#else
        char *priv_typelib_dir = g_build_filename (PKGLIBDIR, "girepository-1.0", NULL);
#endif
        g_irepository_prepend_search_path(priv_typelib_dir);
    g_free (priv_typelib_dir);
    }

    gjs_register_native_module("byteArray", gjs_define_byte_array_stuff);
    gjs_register_native_module("_gi", gjs_define_private_gi_stuff);
    gjs_register_native_module("gi", gjs_define_repo);

    gjs_register_static_modules();
}

static void
gjs_context_tracer(JSTracer *trc, void *data)
{
    GjsContext *gjs_context = reinterpret_cast<GjsContext *>(data);
    JS_CallObjectTracer(trc, &gjs_context->global, "GJS global object");
}

static void
gjs_context_dispose(GObject *object)
{
    GjsContext *js_context;

    js_context = GJS_CONTEXT(object);

    /* Run dispose notifications first, so that anything releasing
     * references in response to this can still get garbage collected */
    G_OBJECT_CLASS(gjs_context_parent_class)->dispose(object);

    if (js_context->context != NULL) {

        gjs_debug(GJS_DEBUG_CONTEXT,
                  "Destroying JS context");

        JS_BeginRequest(js_context->context);

        /* Do a full GC here before tearing down, since once we do
         * that we may not have the JS_GetPrivate() to access the
         * context
         */
        JS_GC(js_context->runtime);
        JS_EndRequest(js_context->context);

        js_context->destroying = true;

        /* Now, release all native objects, to avoid recursion between
         * the JS teardown and the C teardown.  The JSObject proxies
         * still exist, but point to NULL.
         */
        gjs_object_prepare_shutdown(js_context->context);

        if (js_context->auto_gc_id > 0) {
            g_source_remove (js_context->auto_gc_id);
            js_context->auto_gc_id = 0;
        }

        JS_RemoveExtraGCRootsTracer(js_context->runtime, gjs_context_tracer,
                                    js_context);
        js_context->global = NULL;

        for (auto& root : js_context->const_strings)
            delete root;

        /* Tear down JS */
        JS_DestroyContext(js_context->context);
        js_context->context = NULL;
        g_clear_pointer(&js_context->runtime, gjs_runtime_unref);
    }
}

static void
gjs_context_finalize(GObject *object)
{
    GjsContext *js_context;

    js_context = GJS_CONTEXT(object);

    if (js_context->search_path != NULL) {
        g_strfreev(js_context->search_path);
        js_context->search_path = NULL;
    }

    if (js_context->program_name != NULL) {
        g_free(js_context->program_name);
        js_context->program_name = NULL;
    }

    if (gjs_context_get_current() == (GjsContext*)object)
        gjs_context_make_current(NULL);

    g_mutex_lock(&contexts_lock);
    all_contexts = g_list_remove(all_contexts, object);
    g_mutex_unlock(&contexts_lock);

    js_context->global.~Heap();
    G_OBJECT_CLASS(gjs_context_parent_class)->finalize(object);
}

static void
gjs_context_constructed(GObject *object)
{
    GjsContext *js_context = GJS_CONTEXT(object);
    int i;

    G_OBJECT_CLASS(gjs_context_parent_class)->constructed(object);

    js_context->runtime = gjs_runtime_ref();

    JS_AbortIfWrongThread(js_context->runtime);
    js_context->owner_thread = g_thread_self();

    js_context->context = JS_NewContext(js_context->runtime, 8192 /* stack chunk size */);
    if (js_context->context == NULL)
        g_error("Failed to create javascript context");

    for (i = 0; i < GJS_STRING_LAST; i++) {
        js_context->const_strings[i] =
            new JS::PersistentRootedId(js_context->context,
                gjs_intern_string_to_id(js_context->context, const_strings[i]));
    }

    JS_BeginRequest(js_context->context);

    JS_SetGCCallback(js_context->runtime, on_garbage_collect, js_context);

    /* set ourselves as the private data */
    JS_SetContextPrivate(js_context->context, js_context);

    /* setExtraWarnings: Be extra strict about code that might hide a bug */
    if (!g_getenv("GJS_DISABLE_EXTRA_WARNINGS")) {
        gjs_debug(GJS_DEBUG_CONTEXT, "Enabling extra warnings");
        JS::RuntimeOptionsRef(js_context->context).setExtraWarnings(true);
    }

    if (!g_getenv("GJS_DISABLE_JIT")) {
        gjs_debug(GJS_DEBUG_CONTEXT, "Enabling JIT");
        JS::RuntimeOptionsRef(js_context->context)
            .setIon(true)
            .setBaseline(true)
            .setAsmJS(true);
    }

    /* setDontReportUncaught: Don't send exceptions to our error report handler;
     * instead leave them set. This allows us to get at the exception object. */
    JS::ContextOptionsRef(js_context->context).setDontReportUncaught(true);

    JS::RootedObject global(js_context->context,
        gjs_create_global_object(js_context->context));
    if (!global) {
        gjs_log_exception(js_context->context);
        g_error("Failed to initialize global object");
    }

    JSAutoCompartment ac(js_context->context, global);

    new (&js_context->global) JS::Heap<JSObject *>(global);
    JS_AddExtraGCRootsTracer(js_context->runtime, gjs_context_tracer, js_context);

    JS::RootedObject importer(js_context->context,
        gjs_create_root_importer(js_context->context, js_context->search_path ?
                                 js_context->search_path : nullptr));
    if (!importer)
        g_error("Failed to create root importer");

    JS::Value v_importer = gjs_get_global_slot(js_context->context,
                                               GJS_GLOBAL_SLOT_IMPORTS);
    g_assert(((void) "Someone else already created root importer",
              v_importer.isUndefined()));

    gjs_set_global_slot(js_context->context, GJS_GLOBAL_SLOT_IMPORTS,
                        JS::ObjectValue(*importer));

    if (!gjs_define_global_properties(js_context->context, global)) {
        gjs_log_exception(js_context->context);
        g_error("Failed to define properties on global object");
    }

    JS_EndRequest(js_context->context);

    g_mutex_lock (&contexts_lock);
    all_contexts = g_list_prepend(all_contexts, object);
    g_mutex_unlock (&contexts_lock);
}

static void
gjs_context_get_property (GObject     *object,
                          guint        prop_id,
                          GValue      *value,
                          GParamSpec  *pspec)
{
    GjsContext *js_context;

    js_context = GJS_CONTEXT (object);

    switch (prop_id) {
    case PROP_PROGRAM_NAME:
        g_value_set_string(value, js_context->program_name);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
    }
}

static void
gjs_context_set_property (GObject      *object,
                          guint         prop_id,
                          const GValue *value,
                          GParamSpec   *pspec)
{
    GjsContext *js_context;

    js_context = GJS_CONTEXT (object);

    switch (prop_id) {
    case PROP_SEARCH_PATH:
        js_context->search_path = (char**) g_value_dup_boxed(value);
        break;
    case PROP_PROGRAM_NAME:
        js_context->program_name = g_value_dup_string(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
    }
}


GjsContext*
gjs_context_new(void)
{
    return (GjsContext*) g_object_new (GJS_TYPE_CONTEXT, NULL);
}

GjsContext*
gjs_context_new_with_search_path(char** search_path)
{
    return (GjsContext*) g_object_new (GJS_TYPE_CONTEXT,
                         "search-path", search_path,
                         NULL);
}

bool
_gjs_context_destroying (GjsContext *context)
{
    return context->destroying;
}

static gboolean
trigger_gc_if_needed (gpointer user_data)
{
    GjsContext *js_context = GJS_CONTEXT(user_data);
    js_context->auto_gc_id = 0;
    gjs_gc_if_needed(js_context->context);
    return G_SOURCE_REMOVE;
}

void
_gjs_context_schedule_gc_if_needed (GjsContext *js_context)
{
    if (js_context->auto_gc_id > 0)
        return;

    js_context->auto_gc_id = g_idle_add_full(G_PRIORITY_LOW,
                                             trigger_gc_if_needed,
                                             js_context, NULL);
}

void
_gjs_context_exit(GjsContext *js_context,
                  uint8_t     exit_code)
{
    g_assert(!js_context->should_exit);
    js_context->should_exit = true;
    js_context->exit_code = exit_code;
}

bool
_gjs_context_should_exit(GjsContext *js_context,
                         uint8_t    *exit_code_p)
{
    if (exit_code_p != NULL)
        *exit_code_p = js_context->exit_code;
    return js_context->should_exit;
}

static void
context_reset_exit(GjsContext *js_context)
{
    js_context->should_exit = false;
    js_context->exit_code = 0;
}

bool
_gjs_context_get_is_owner_thread(GjsContext *js_context)
{
    return js_context->owner_thread == g_thread_self();
}

/**
 * gjs_context_maybe_gc:
 * @context: a #GjsContext
 * 
 * Similar to the Spidermonkey JS_MaybeGC() call which
 * heuristically looks at JS runtime memory usage and
 * may initiate a garbage collection. 
 *
 * This function always unconditionally invokes JS_MaybeGC(), but
 * additionally looks at memory usage from the system malloc()
 * when available, and if the delta has grown since the last run
 * significantly, also initiates a full JavaScript garbage
 * collection.  The idea is that since GJS is a bridge between
 * JavaScript and system libraries, and JS objects act as proxies
 * for these system memory objects, GJS consumers need a way to
 * hint to the runtime that it may be a good idea to try a
 * collection.
 *
 * A good time to call this function is when your application
 * transitions to an idle state.
 */ 
void
gjs_context_maybe_gc (GjsContext  *context)
{
    gjs_maybe_gc(context->context);
}

/**
 * gjs_context_gc:
 * @context: a #GjsContext
 * 
 * Initiate a full GC; may or may not block until complete.  This
 * function just calls Spidermonkey JS_GC().
 */ 
void
gjs_context_gc (GjsContext  *context)
{
    JS_GC(context->runtime);
}

/**
 * gjs_context_get_all:
 *
 * Returns a newly-allocated list containing all known instances of #GjsContext.
 * This is useful for operating on the contexts from a process-global situation
 * such as a debugger.
 *
 * Return value: (element-type GjsContext) (transfer full): Known #GjsContext instances
 */
GList*
gjs_context_get_all(void)
{
  GList *result;
  GList *iter;
  g_mutex_lock (&contexts_lock);
  result = g_list_copy(all_contexts);
  for (iter = result; iter; iter = iter->next)
    g_object_ref((GObject*)iter->data);
  g_mutex_unlock (&contexts_lock);
  return result;
}

/**
 * gjs_context_get_native_context:
 *
 * Returns a pointer to the underlying native context.  For SpiderMonkey, this
 * is a JSContext *
 */
void*
gjs_context_get_native_context (GjsContext *js_context)
{
    g_return_val_if_fail(GJS_IS_CONTEXT(js_context), NULL);
    return js_context->context;
}

bool
gjs_context_eval(GjsContext   *js_context,
                 const char   *script,
                 gssize        script_len,
                 const char   *filename,
                 int          *exit_status_p,
                 GError      **error)
{
    bool ret = false;

    JSAutoCompartment ac(js_context->context, js_context->global);
    JSAutoRequest ar(js_context->context);

    g_object_ref(G_OBJECT(js_context));

    JS::RootedValue retval(js_context->context);
    if (!gjs_eval_with_scope(js_context->context, JS::NullPtr(), script,
                             script_len, filename, &retval)) {
        uint8_t code;
        if (_gjs_context_should_exit(js_context, &code)) {
            /* exit_status_p is public API so can't be changed, but should be
             * uint8_t, not int */
            *exit_status_p = code;
            g_set_error(error, GJS_ERROR, GJS_ERROR_SYSTEM_EXIT,
                        "Exit with code %d", code);
            goto out;  /* Don't log anything */
        }

        gjs_log_exception(js_context->context);
        g_set_error(error,
                    GJS_ERROR,
                    GJS_ERROR_FAILED,
                    "JS_EvaluateScript() failed");
        /* No exit code from script, but we don't want to exit(0) */
        *exit_status_p = 1;
        goto out;
    }

    if (exit_status_p) {
        if (retval.isInt32()) {
            int code = retval.toInt32();
            gjs_debug(GJS_DEBUG_CONTEXT,
                      "Script returned integer code %d", code);
            *exit_status_p = code;
        } else {
            /* Assume success if no integer was returned */
            *exit_status_p = 0;
        }
    }

    ret = true;

 out:
    g_object_unref(G_OBJECT(js_context));
    context_reset_exit(js_context);
    return ret;
}

bool
gjs_context_eval_file(GjsContext    *js_context,
                      const char    *filename,
                      int           *exit_status_p,
                      GError       **error)
{
    char     *script = NULL;
    gsize    script_len;
    bool ret = true;

    GFile *file = g_file_new_for_commandline_arg(filename);

    if (!g_file_query_exists(file, NULL)) {
        ret = false;
        goto out;
    }

    if (!g_file_load_contents(file, NULL, &script, &script_len, NULL, error)) {
        ret = false;
        goto out;
    }

    if (!gjs_context_eval(js_context, script, script_len, filename, exit_status_p, error)) {
        ret = false;
        goto out;
    }

out:
    g_free(script);
    g_object_unref(file);
    return ret;
}

bool
gjs_context_define_string_array(GjsContext  *js_context,
                                const char    *array_name,
                                gssize         array_length,
                                const char   **array_values,
                                GError       **error)
{
    JSAutoCompartment ac(js_context->context, js_context->global);
    JSAutoRequest ar(js_context->context);

    JS::RootedObject global_root(js_context->context, js_context->global);
    if (!gjs_define_string_array(js_context->context,
                                 global_root,
                                 array_name, array_length, array_values,
                                 JSPROP_READONLY | JSPROP_PERMANENT)) {
        gjs_log_exception(js_context->context);
        g_set_error(error,
                    GJS_ERROR,
                    GJS_ERROR_FAILED,
                    "gjs_define_string_array() failed");
        return false;
    }

    return true;
}

static GjsContext *current_context;

GjsContext *
gjs_context_get_current (void)
{
    return current_context;
}

void
gjs_context_make_current (GjsContext *context)
{
    g_assert (context == NULL || current_context == NULL);

    current_context = context;
}

/* It's OK to return JS::HandleId here, to avoid an extra root, with the
 * caveat that you should not use this value after the GjsContext has
 * been destroyed. */
JS::HandleId
gjs_context_get_const_string(JSContext      *context,
                             GjsConstString  name)
{
    GjsContext *gjs_context = (GjsContext *) JS_GetContextPrivate(context);
    return *gjs_context->const_strings[name];
}

/**
 * gjs_get_import_global:
 * @context: a #JSContext
 *
 * Gets the "import global" for the context's runtime. The import
 * global object is the global object for the context. It is used
 * as the root object for the scope of modules loaded by GJS in this
 * runtime, and should also be used as the globals 'obj' argument passed
 * to JS_InitClass() and the parent argument passed to JS_ConstructObject()
 * when creating a native classes that are shared between all contexts using
 * the runtime. (The standard JS classes are not shared, but we share
 * classes such as GObject proxy classes since objects of these classes can
 * easily migrate between contexts and having different classes depending
 * on the context where they were first accessed would be confusing.)
 *
 * Return value: the "import global" for the context's
 *  runtime. Will never return %NULL while GJS has an active context
 *  for the runtime.
 */
JSObject*
gjs_get_import_global(JSContext *context)
{
    GjsContext *gjs_context = (GjsContext *) JS_GetContextPrivate(context);
    return gjs_context->global;
}
