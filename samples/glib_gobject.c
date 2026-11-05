#include <glib-object.h>
#include <stdio.h>
typedef struct { GObject parent; int v; } Foo; typedef struct { GObjectClass parent; } FooClass;
G_DEFINE_TYPE(Foo, foo, G_TYPE_OBJECT)
enum { SIG_CHANGED, SIG_COMPUTE, N_SIG }; static guint sigs[N_SIG];
static void foo_init(Foo *f) { f->v = 1; }
static void foo_class_init(FooClass *k) {
  sigs[SIG_CHANGED] = g_signal_new("changed", G_TYPE_FROM_CLASS(k), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_INT, G_TYPE_STRING);   // no marshaller: libffi generic marshal
  sigs[SIG_COMPUTE] = g_signal_new("compute", G_TYPE_FROM_CLASS(k), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_DOUBLE, 3, G_TYPE_DOUBLE, G_TYPE_INT64, G_TYPE_BOOLEAN);
}
static void on_changed(Foo *f, int n, const char *s, gpointer ud) { printf("changed %d %s %s\n", n, s, (const char *)ud); }
static double on_compute(Foo *f, double d, gint64 i, gboolean b, gpointer ud) { return d * (double)i + (b ? 1 : 0); }
static gboolean idle(gpointer p) { g_main_loop_quit(p); return G_SOURCE_REMOVE; }
int main(void) {
  Foo *f = g_object_new(foo_get_type(), NULL);
  g_signal_connect(f, "changed", G_CALLBACK(on_changed), "ud"); g_signal_connect(f, "compute", G_CALLBACK(on_compute), NULL);
  g_signal_emit(f, sigs[SIG_CHANGED], 0, 42, "hello");
  double r = 0; g_signal_emit(f, sigs[SIG_COMPUTE], 0, 2.5, (gint64)4, TRUE, &r); printf("compute %.1f\n", r);
  GValue v = G_VALUE_INIT; g_value_init(&v, G_TYPE_DOUBLE); g_value_set_double(&v, 7.5); GValue out = G_VALUE_INIT; g_value_init(&out, G_TYPE_STRING); g_value_transform(&v, &out); printf("transform %s\n", g_value_get_string(&out));
  GVariant *gv = g_variant_new_parsed("{'a': <[1, 2, 3]>, 'b': <('x', 2.5)>}"); gchar *s = g_variant_print(gv, FALSE); printf("variant %s\n", s); g_free(s);
  GHashTable *h = g_hash_table_new(g_str_hash, g_str_equal); g_hash_table_insert(h, "k", GINT_TO_POINTER(5)); printf("hash %d\n", GPOINTER_TO_INT(g_hash_table_lookup(h, "k")));
  GMainLoop *ml = g_main_loop_new(NULL, FALSE); g_timeout_add(30, idle, ml); g_main_loop_run(ml); printf("loop done\n");
  GError *err = NULL; gchar *cont = NULL; g_file_get_contents("/nonexistent", &cont, NULL, &err); printf("gerror %d\n", err != NULL); g_clear_error(&err);
  printf("utf8 %ld %s\n", g_utf8_strlen("héllo", -1), g_utf8_strup("straße", -1));
  return 0;
}
