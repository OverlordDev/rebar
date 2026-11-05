#include <libxml/parser.h>
#include <libxml/SAX2.h>
#include <stdio.h>
#include <string.h>
static void start(void *ctx, const xmlChar *ln, const xmlChar *prefix, const xmlChar *uri, int nns, const xmlChar **ns, int nattr, int ndef, const xmlChar **attrs) {
  printf("start %s ns=%d attrs=%d def=%d", ln, nns, nattr, ndef);
  for (int i = 0; i < nattr; i++) printf(" %s=%.*s", attrs[i * 5], (int)(attrs[i * 5 + 4] - attrs[i * 5 + 3]), attrs[i * 5 + 3]);
  printf("\n");
}
int main(void) {
  xmlSAXHandler h; memset(&h, 0, sizeof h); h.initialized = XML_SAX2_MAGIC; h.startElementNs = start;
  const char *doc = "<r xmlns:p='u'><a x='1' y='22'/><p:b/></r>";
  int rc = xmlSAXUserParseMemory(&h, NULL, doc, (int)strlen(doc)); printf("rc %d\n", rc);
  xmlParserCtxtPtr ctx = xmlCreatePushParserCtxt(&h, NULL, NULL, 0, "x.xml"); xmlParseChunk(ctx, doc, (int)strlen(doc), 1); xmlFreeParserCtxt(ctx);
  return 0;
}
