#ifndef QC_GRAPH_JSON_BUILDER_H
#define QC_GRAPH_JSON_BUILDER_H

void vQuickConnectGraphsStart(void);
void vQuickConnectGraphsAddGraph(const char *pcName, const char *pcUnit, 
    const char *pcFormat, ...);
char *pcQuickConnectGraphsEnd(void);

#endif /* QC_GRAPH_JSON_BUILDER_H */