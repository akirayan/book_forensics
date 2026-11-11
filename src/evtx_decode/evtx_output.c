
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#include "evtx_output.h"
#include "evtx_xmltree.h"



static bool tree_matched_eventid(XML_TREE *tree, uint32_t target_eventid)
{
    if (target_eventid == 0)
        return true;

    char *id_text = xml_event_id(tree);
    uint32_t id = (uint32_t)atoi(id_text);

    return id == target_eventid;
}


static bool tree_matched(XML_TREE *tree, uint32_t output_mode)
{
    return tree_matched_eventid(tree, GET_EVTID(output_mode));
}


void output_xmltree(XML_TREE *xtree, uint32_t output_mode) 
{
    if (!tree_matched(xtree, output_mode))
        return;

    if (IS_OUT_DEFAULT(output_mode)) {
       xml_dump_tree_default(xtree);
    }

    if (CHECK_OUTMODE(output_mode, OUT_CSV)) {
       xml_dump_tree_csv(xtree);
    }

    if (CHECK_OUTMODE(output_mode, OUT_XML)) {
       xml_dump_tree_xml(xtree);
    }

    if (CHECK_OUTMODE(output_mode, OUT_TXT)) {
       xml_dump_tree_txt(xtree);
    }

    if (CHECK_OUTMODE(output_mode, OUT_DEBUG)) {
       xml_dump_tree_debug(xtree);
    }

}
