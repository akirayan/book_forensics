/*
============================================================
XML Tree Builder Library
------------------------------------------------------------
Purpose:
    Generic XML tree builder with cursor-based navigation.

Design model:
    Tree owns elements.
    Elements own attributes.
    Attributes own strings.

Usage model:
    xml_push_element()
    xml_begin_attribute()
    xml_end_attribute()
    xml_set_element()
    xml_pop_element()

Ownership:
    xml_free_tree() frees entire tree.
============================================================
*/

// evtx_xmltree.h
//
// Minimal XML tree library for EVTX BinXML decoding.
// Purpose:
//   - Represent EVTX BinXML as text-style XML
//   - NOT a full XML implementation
//   - Element-only tree (no comments, CDATA, PI, etc.)
//
// Designed for forensic tools and deterministic output.
//

#ifndef EVTX_XMLTREE_H
#define EVTX_XMLTREE_H

#include <stdint.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

// ------------------------------------------------------------
// XML Attribute
// ------------------------------------------------------------
// Example:
//   Name="Microsoft-Windows-Servicing"
//   xmlns="http://schemas.microsoft.com/..."
//
typedef struct _XML_ATTRIBUTE {
    char *name;     // attribute name
    char *value;    // attribute value (NULL if none)
    uint8_t value_type;
} XML_ATTRIBUTE;


// ------------------------------------------------------------
// XML Element (node)
// ------------------------------------------------------------
// Represents a single XML element.
// - Text content is stored directly in 'text'
// - Children are ordered (as in EVTX)
// - Namespace handling is string-based only (xmlns as attribute)
//
typedef struct _XML_ELEMENT {
    char *name;                 // element name (e.g. "Event", "System")
    char *text;                 // text content (NULL if none)
    uint8_t text_type;

    uint16_t attr_count;
    XML_ATTRIBUTE *attrs;

    // tree links
    struct _XML_ELEMENT *parent;
    struct _XML_ELEMENT *first_child;
    struct _XML_ELEMENT *last_child;
    struct _XML_ELEMENT *next_sibling;
} XML_ELEMENT;


// ------------------------------------------------------------
// XML Tree (top-level owner)
// ------------------------------------------------------------
// One XML_TREE corresponds to one EVTX record.
//

typedef struct _XML_TREE {
    XML_ELEMENT *root;          // root element (<Event>)
    XML_ELEMENT *current;       // internal use, always points to the current element
    XML_ATTRIBUTE *current_attr;// internal use, always points to the attribute
} XML_TREE;


// ------------------------------------------------------------
// Public APIs
// ------------------------------------------------------------



// ============================================================
// XML TREE Lifecycle API
// ============================================================
XML_TREE    *xml_new_tree(void);
void         xml_free_tree(XML_TREE *tree);
// Output
void         xml_dump_tree_xml(XML_TREE *tree);
void         xml_dump_tree_compact(XML_TREE *tree);
void         xml_dump_tree_txt(XML_TREE *tree);
void         xml_dump_tree_default(XML_TREE *tree);
void         xml_dump_tree_debug(XML_TREE *tree);
void         xml_dump_tree_csv(XML_TREE *tree);

// ============================================================
// Element Navigation API (cursor / structural control)
// ============================================================
XML_ELEMENT *xml_push_element(XML_TREE *tree, const char *name);
XML_ELEMENT *xml_pop_element(XML_TREE *tree);
XML_ELEMENT *xml_current_element(XML_TREE *tree);
void         xml_add_child(XML_TREE *tree, XML_ELEMENT *child);

// ============================================================
// Element Content API
// ============================================================
XML_ELEMENT *xml_new_element(const char *name);
int          xml_set_element(XML_ELEMENT *e, const char *text, uint8_t text_type);
char        *xml_event_id(XML_TREE *tree);

// ============================================================
// Attribute Lifecycle API
// ============================================================
XML_ATTRIBUTE *xml_new_attribute(const char *name);
int            xml_set_attribute(XML_ATTRIBUTE *a, const char *value, uint8_t value_type);
int            xml_add_attribute(XML_TREE *tree, XML_ELEMENT *elem, XML_ATTRIBUTE *attr);
// state-machine helpers
XML_ATTRIBUTE *xml_begin_attribute(XML_TREE *tree, const char *name);
int            xml_end_attribute(XML_TREE *tree, const char *value, uint8_t value_type);
XML_ATTRIBUTE *xml_current_attribute(XML_TREE *tree);

// ============================================================
// Search / Query API
// ============================================================
XML_ELEMENT *xml_find_child(XML_TREE *tree, const char *name);
XML_ELEMENT *xml_find_path(XML_TREE *tree, const char *path);





#ifdef __cplusplus
}
#endif

#endif /* EVTX_XMLTREE_H */
