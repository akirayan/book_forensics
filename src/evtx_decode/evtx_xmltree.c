
//
// Minimal XML tree library for EVTX BinXML decoding.
// NOT a full XML implementation.
//
// Memory model:
//   - xml_new_tree() allocates an empty tree (root=NULL)
//   - xml_new_element() allocates a node and duplicates strings
//   - xml_add_attribute() appends to a dynamic array
//   - xml_add_child() appends child in O(1) using last_child
//   - xml_free_tree() frees everything recursively
//

#include "evtx_xmltree.h"

#include <string.h>   // strlen, memcpy
#include <stdio.h>    // optional (not required), but handy for debugging
#include <stdbool.h>    // optional (not required), but handy for debugging

#define BINXML_VALUE_NULL 0x00


///////////// Internal functions ///////////////////////////
static void xml_dump_indent(int depth);
static char *get_normalized_text(const char *in);
static void dump_attrs(XML_ELEMENT *e);
static char *xml_strdup(const char *s);
static void free_element(XML_ELEMENT *elem);
static void dump_element_xml(XML_ELEMENT *e, int depth);
static void dump_element_compact_rec(XML_ELEMENT *e);
static void dump_element_compact(XML_ELEMENT *elem);
static XML_ELEMENT * find_child_from(XML_ELEMENT *parent, const char *name);
static XML_ELEMENT * find_element_by_path(XML_ELEMENT *root, const char *path);
static char *element_text(XML_TREE *tree, const char *path);
static char *attribute_value(XML_TREE *tree, const char *path, const char *attr_name);
static const char * safe_str(const char *s);
static void csv_escape_and_print(const char *s);
static const char * level_to_name(const char *level_str);
static bool check_normaliz_needed(char *text) ;
static char *get_csv_build_strings(XML_TREE *tree);
static void dump_element_debug(XML_ELEMENT *e, int depth) ;
static void dump_element_txt(XML_ELEMENT *e, const char *parent_path);




// -------------------------
// Internal helpers
// -------------------------

static void xml_dump_indent(int depth)
{
    for (int i = 0; i < depth; i++)
        fputs("  ", stdout);
}


static char *get_normalized_text(const char *in)
{
    if (!in) return NULL;

    size_t len = strlen(in);

    char *out = malloc(len + 1);
    if (!out) return NULL;

    size_t j = 0;
    int prev_space = 0;

    for (size_t i = 0; i < len; i++) {
        char c = in[i];
        if (c == '\r' || c == '\n' || c == '\t' || c == ' ') {
            if (!prev_space) {
                out[j++] = ' ';
                prev_space = 1;
            }
        } else {
            out[j++] = c;
            prev_space = 0;
        }
    }
    out[j] = '\0';
    return out;
}


static void dump_attrs(XML_ELEMENT *e)
{
    for (uint16_t i = 0; i < e->attr_count; i++) {
        XML_ATTRIBUTE *a = &e->attrs[i];

        // NULL value type は出力しない（EventViewerの <Correlation /> など）
        if (a->value_type == BINXML_VALUE_NULL) {
            //printf("\nDEBUG: Attribute Name:%s Value:\"%s\" value_type=0x%x\n", a->name, a->value, a->value_type);
            continue;
        }

        // 値が無い/壊れてるケースも念のため skip
        if (!a->name || !a->value) {
            continue;
        }

        printf(" %s=\"%s\"", a->name, a->value);
    }
}




static char *xml_strdup(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s);
    char *p = (char *)malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n + 1);
    return p;
}


static void free_element(XML_ELEMENT *elem)
{
    if (!elem) return;

    // free children
    XML_ELEMENT *child = elem->first_child;
    while (child) {
        XML_ELEMENT *next = child->next_sibling;
        free_element(child);
        child = next;
    }

    // free attributes
    for (uint16_t i = 0; i < elem->attr_count; i++) {
        free(elem->attrs[i].name);
        free(elem->attrs[i].value);
    }
    free(elem->attrs);

    // free strings
    free(elem->name);
    free(elem->text);

    // free element itself
    free(elem);
    elem = (XML_ELEMENT *)NULL;
}


static void dump_element_xml(XML_ELEMENT *e, int depth)
{
    xml_dump_indent(depth);
    printf("<%s", e->name);
    dump_attrs(e);

    bool has_children = (e->first_child != NULL);
    bool has_text = (e->text != NULL && e->text[0] != '\0');

    // Case 1: empty element
    if (!has_children && !has_text)
    {
        printf(" />\n");
        return;
    }

    // Case 2: leaf element with text → single line
    if (!has_children && has_text)
    {
        printf(">%s</%s>\n", e->text, e->name);
        return;
    }

    // Case 3: element with children
    printf(">\n");

    // print text first if exists
    if (has_text)
    {
        xml_dump_indent(depth + 1);
        printf("%s\n", e->text);
    }

    // print children
    for (XML_ELEMENT *c = e->first_child; c; c = c->next_sibling)
    {
        dump_element_xml(c, depth + 1);
    }

    xml_dump_indent(depth);
    printf("</%s>\n", e->name);
}



static void dump_element_compact_rec(XML_ELEMENT *e)
{
    if (!e) return;

    printf("<%s", e->name);
    dump_attrs(e);

    if (!e->first_child && !e->text) {
        printf("/>");
        return;
    }

    printf(">");

    if (e->text)
        fputs(e->text, stdout);

    for (XML_ELEMENT *c = e->first_child; c; c = c->next_sibling)
        dump_element_compact_rec(c);

    printf("</%s>", e->name);
}



static void dump_element_compact(XML_ELEMENT *elem)
{
    dump_element_compact_rec(elem);
    putchar('\n');
}



// -------------------------
// Public APIs for XML_ATTRIBUTE
// -------------------------

XML_ATTRIBUTE *xml_new_attribute(const char *name)
{
    if (!name)
        return NULL;

    XML_ATTRIBUTE *a = (XML_ATTRIBUTE *)calloc(1, sizeof(XML_ATTRIBUTE));
    if (!a)
        return NULL;

    a->name = xml_strdup(name);
    if (!a->name) {
        free(a);
        return NULL;
    }

    a->value = NULL;
    a->value_type = 0xFF;   // undefined yet

    return a;
}

int xml_set_attribute(XML_ATTRIBUTE *attr, const char *value, uint8_t value_type)
{
    if (!attr)
        return -1;

    // free previous value if any
    if (attr->value) {
        free(attr->value);
        attr->value = NULL;
    }

    attr->value_type = value_type;

    if (value_type != 0x00 && value) {   // 0x00 = NULL type
        attr->value = xml_strdup(value);
        if (!attr->value)
            return -1;
    } else {
        attr->value = NULL;
    }

    return 0;
}

XML_ATTRIBUTE *xml_current_attribute(XML_TREE *tree)
{
    if (!tree)
        return NULL;

    return tree->current_attr;
}


static XML_ELEMENT * find_child_from(XML_ELEMENT *parent, const char *name)
{
    if (!parent || !name)
        return NULL;

    for (XML_ELEMENT *c = parent->first_child; c; c = c->next_sibling)
    {
        if (strcmp(c->name, name) == 0)
            return c;
    }

    return NULL;
}







// -------------------------
// Public APIs for XML_ELEMENT
// -------------------------

XML_ELEMENT *xml_new_element(const char *name)
{
    if (!name) return NULL;

    XML_ELEMENT *e = (XML_ELEMENT *)calloc(1, sizeof(XML_ELEMENT));
    if (!e) return NULL;

    e->name = xml_strdup(name);
    if (!e->name) {
        free(e);
        return NULL;
    }

    // text=NULL, attrs=NULL, counts=0, links=NULL by calloc
    return e;
}


int xml_set_element(XML_ELEMENT *elem, const char *text, uint8_t text_type)
{
    if (!elem) return -1;

    if (elem->text) {
        free(elem->text);
        elem->text = NULL;
    }

    elem->text_type = text_type;

    if (text_type == BINXML_VALUE_NULL || !text) {
        elem->text = NULL;
        return 0;
    }

    elem->text = xml_strdup(text);
    return (elem->text != NULL) ? 0 : -1;
}

XML_ELEMENT *xml_push_element(XML_TREE *tree, const char *name)
{
    if (!tree || !name)
        return NULL;

    XML_ELEMENT *e = xml_new_element(name);
    if (!e)
        return NULL;

    if (!tree->root) {
        tree->root = e;
    } else if (tree->current) {
        xml_add_child(tree, e);
    }

    tree->current = e;

    return e;
}

XML_ELEMENT *xml_pop_element(XML_TREE *tree)
{
    if (!tree || !tree->current)
        return NULL;

    tree->current = tree->current->parent;

    return tree->current;
}

XML_ELEMENT *xml_current_element(XML_TREE *tree)
{
    if (!tree)
        return NULL;

    return tree->current;
}


int xml_add_attribute(XML_TREE *tree, XML_ELEMENT *elem, XML_ATTRIBUTE *attr)
{
    if (!tree || !elem || !attr)
        return -1;

    XML_ATTRIBUTE *new_attrs =
        (XML_ATTRIBUTE *)realloc(elem->attrs,
            (elem->attr_count + 1) * sizeof(XML_ATTRIBUTE));

    if (!new_attrs)
        return -1;

    elem->attrs = new_attrs;

    elem->attrs[elem->attr_count] = *attr;

    elem->attr_count++;

    free(attr);

    return 0;
}

XML_ATTRIBUTE *xml_begin_attribute(XML_TREE *tree, const char *name)
{
    if (!tree || !tree->current)
        return NULL;

    XML_ATTRIBUTE *attr = xml_new_attribute(name);
    if (!attr) return NULL;
    tree->current_attr = attr;
    return attr;
}

int xml_end_attribute(XML_TREE *tree, const char *value, uint8_t value_type)
{
    if (!tree || !tree->current || !tree->current_attr)
        return -1;
    xml_set_attribute(tree->current_attr, value, value_type);
    xml_add_attribute(tree, tree->current, tree->current_attr);
    tree->current_attr = NULL;
    return 0;
}





// -------------------------
// Public APIs for XML_TREE
// -------------------------
XML_TREE *xml_new_tree(void)
{
    XML_TREE *tree = (XML_TREE *)calloc(1, sizeof(XML_TREE));
    if (!tree)
        return NULL;

    tree->root = NULL;

    // initialize parser state
    tree->current = NULL;
    tree->current_attr = NULL;

    return tree;
}

void xml_free_tree(XML_TREE *tree)
{
    if (!tree) return;

    if (tree->root)
        free_element(tree->root);

    tree->current = NULL;
    tree->current_attr = NULL;

    free(tree);
}


void xml_add_child(XML_TREE *tree, XML_ELEMENT *child)
{
    if (!tree || !child)
        return;

    /* root case */
    if (!tree->root) {
        tree->root = child;
        return;
    }

    XML_ELEMENT *parent = tree->current;
    if (!parent)
        return;

    child->parent = parent;

    if (!parent->first_child)
        parent->first_child = child;
    else
        parent->last_child->next_sibling = child;

    parent->last_child = child;

    /* DO NOT change tree->current here */
}


XML_ELEMENT * xml_find_child(XML_TREE *tree, const char *name)
{
    if (!tree || !tree->current || !name)
        return NULL;

    return find_child_from(tree->current, name);
}

void xml_dump_tree_xml(XML_TREE *tree)
{
    if (!tree || !tree->root) return;
    dump_element_xml(tree->root, 0);
}

void xml_dump_tree_compact(XML_TREE *tree)
{
    if (!tree || !tree->root) return;
    dump_element_compact(tree->root);
}







static XML_ELEMENT * find_element_by_path(XML_ELEMENT *root, const char *path)
{
    if (!root || !path) return NULL;

    size_t n = strlen(path);
    if (n >= 1024) return NULL;            // まずガード（好きな上限に）
    char buf[1024];
    memcpy(buf, path, n + 1);

    char *saveptr = NULL;
    char *token = strtok_r(buf, ".", &saveptr);
    XML_ELEMENT *current = root;

    if (!token || strcmp(current->name, token) != 0)
        return NULL;

    while ((token = strtok_r(NULL, ".", &saveptr)) != NULL) {
        XML_ELEMENT *child = current->first_child;
        while (child && strcmp(child->name, token) != 0)
            child = child->next_sibling;

        if (!child) return NULL;
        current = child;
    }

    return current;
}


XML_ELEMENT *xml_find_path(XML_TREE *tree, const char *path)
{
    if (!tree || !tree->root || !path)
        return NULL;
     return find_element_by_path(tree->root, path);
}


static char *element_text(XML_TREE *tree, const char *path)
{
    XML_ELEMENT *e =
        find_element_by_path(tree->root, path);

    if (!e || !e->text)
        return NULL;

    //return xml_strdup(e->text);
    return e->text;
}


static char *attribute_value(XML_TREE *tree, const char *path, const char *attr_name)
{
    XML_ELEMENT *e =
        find_element_by_path(tree->root, path);

    if (!e)
        return NULL;

    for (int i = 0; i < e->attr_count; i++)
    {
        XML_ATTRIBUTE *a = &e->attrs[i];

        if (strcmp(a->name, attr_name) == 0)
        {
            if (!a->value)
                return NULL;

            //return xml_strdup(a->value);
            return a->value;
        }
    }

    return NULL;
}





char *xml_event_id(XML_TREE *tree)
{
    return element_text(tree, "Event.System.EventID");
}




void xml_dump_tree_default(XML_TREE *tree)
{
    if (!tree || !tree->root) return;

    char *recID = element_text(tree, "Event.System.EventRecordID");
    char *created = attribute_value(tree, "Event.System.TimeCreated", "SystemTime");
    char *evtID = element_text(tree, "Event.System.EventID");
    char *chan = element_text(tree, "Event.System.Channel");
    char *host = element_text(tree, "Event.System.Computer");


    printf("Record#%s\tcreated=%s\tevent_id=%s\tchannel=%s\thost=%s\n",
        recID ? recID : "(null)",
        created ? created : "(null)",
        evtID ? evtID : "(null)",
        chan ? chan : "(null)",
        host ? host : "(null)"
         );
}







//=================CSV MODE DUMP ====================
// production-quality CSV output
// LogParser compatible format

static const char * safe_str(const char *s)
{
    return s ? s : "-";
}

static void csv_escape_and_print(const char *s)
{
    if (!s) {
        printf("-");
        return;
    }

    int needs_quote = 0;

    for (const char *p = s; *p; p++) {
        if (*p == ',' || *p == '"' || *p == '\n' || *p == '\r') {
            needs_quote = 1;
            break;
        }
    }

    if (!needs_quote) {
        printf("%s", s);
        return;
    }

    putchar('"');

    for (const char *p = s; *p; p++) {
        if (*p == '"')
            putchar('"');   // escape double quote
        putchar(*p);
    }

    putchar('"');
}

static const char * level_to_name(const char *level_str)
{
    if (!level_str) return "-";

    int level = atoi(level_str);

    switch(level)
    {
        case 0: return "Information";
        case 1: return "Critical";
        case 2: return "Error";
        case 3: return "Warning";
        case 4: return "Information";
        case 5: return "Verbose";
        default: return "-";
    }
}




static bool check_normaliz_needed(char *text) 
{
    // check if NEWLINE in the text, maybe add other chars
    char *p = strstr(text, "\r");
    return p ? true : false;
}


static char *get_csv_build_strings(XML_TREE *tree)
{
    XML_ELEMENT *eventdata = xml_find_path(tree, "Event.EventData");
    XML_ELEMENT *userdata = NULL;

    char out[4096];
    size_t out_size = sizeof(out);
    out[0] = '\0';

    char *ptr = NULL;

    // Some events using EventData, but some using UserData, we try both
    if (!eventdata) 
        userdata = xml_find_path(tree, "Event.UserData");

    if (eventdata) {
        XML_ELEMENT *c = eventdata->first_child;
        while (c) {
            if (c->text && c->text[0] != '\0') {
                if (out[0] != '\0') {
                    strncat(out, "|", out_size - strlen(out) - 1);
                } 
                strncat(out, c->text, out_size - strlen(out) - 1);
            }
            c = c->next_sibling;
        }
    } else if (userdata) {
        XML_ELEMENT *c= userdata->first_child;
        while (c) {
            XML_ELEMENT *m = c->first_child;  // mako: child of child
            while (m) {
                if (m->text && m->text[0] != '\0') {

                    if (out[0] != '\0') {
                        strncat(out, "|", out_size - strlen(out) - 1);
                    } 
                    strncat(out, m->text, out_size - strlen(out) - 1);
                }
                m = m->next_sibling;
            }
            c = c->next_sibling;
        }

    } else {
        ;;
    }


    // check if needed to be normalized
    if (check_normaliz_needed(out)) {
        ptr = get_normalized_text(out);
    } else {
        ptr = malloc(strlen(out) + 1);
        strcpy(ptr, out);
    }


    return ptr;
}






void xml_dump_tree_csv(XML_TREE *tree)
{
    if (!tree || !tree->root)
        return;

    const char *record_id = element_text(tree, "Event.System.EventRecordID");
    const char *time = attribute_value(tree, "Event.System.TimeCreated", "SystemTime");
    const char *event_id = element_text(tree, "Event.System.EventID");
    const char *level = element_text(tree, "Event.System.Level");
    const char *task = element_text(tree, "Event.System.Task");
    const char *provider = attribute_value(tree, "Event.System.Provider", "Name");
    const char *computer = element_text(tree, "Event.System.Computer");
    const char *sid = attribute_value(tree, "Event.System.Security", "UserID");

    const char *event_type_name = level_to_name(level);

    char *strings = get_csv_build_strings(tree);


    //
    // CSV Output
    //
    csv_escape_and_print(safe_str("-")); printf(",");  // no filename 
    csv_escape_and_print(safe_str(record_id)); printf(",");
    csv_escape_and_print(safe_str(time)); printf(",");
    csv_escape_and_print(safe_str(time)); printf(",");
    csv_escape_and_print(safe_str(event_id)); printf(",");
    csv_escape_and_print(safe_str(level)); printf(",");
    csv_escape_and_print(safe_str(event_type_name)); printf(",");
    csv_escape_and_print(safe_str(task)); printf(",");
    csv_escape_and_print(safe_str(provider)); printf(",");
    csv_escape_and_print(safe_str(strings)); printf(",");
    csv_escape_and_print(safe_str(computer)); printf(",");
    csv_escape_and_print(safe_str(sid));

    // do not forget to free 
    free(strings);

    printf("\n");
}


static void dump_element_debug(XML_ELEMENT *e, int depth) 
{
    if (!e) return;

    char SPACE[80];
    SPACE[0] = 0x00;
    for (int i=0; i < depth; i++) { 
        strcat(SPACE, "    ");
    }

    // e->text may have \r code, eg Security evtID 4672
    printf("%s%s text=%s attrs=%d ", SPACE, e->name, e->text, e->attr_count);

    printf("e=%p p=%p c=%p n=%p\n", e, e->parent, e->first_child, e->next_sibling);
    for (int i=0; i < e->attr_count; i++) {
        XML_ATTRIBUTE *a = &e->attrs[i];
        printf("%s      attr%d: %s=%s\n", SPACE, i + 1, a->name, a->value);
    }
    
    // children
    for (XML_ELEMENT *c = e->first_child; c; c = c->next_sibling) {
        dump_element_debug(c, depth + 1); 
    }   
}

void xml_dump_tree_debug(XML_TREE *tree)
{
     if (!tree || !tree->root) return;
     dump_element_debug(tree->root, 0);
}






/////////////////////// Type of Elements /////////////////
// # child text  attr  eg  
// 1 NO    NO    NO    <Correlation />
// 2 NO    NO    YES   <Provider Name=... GUID=... />, <TimeCreated SystemTime=... />
// 3 NO    YES   NO    <EventID>4624</EventID>
// 4 NO    YES   YES   <Data Name=LogonID>0x37</Data> 
// 5 YES   NO    NO    <System>
// 6 YES   NO    YES   <Event>
// 7 YES   YES   NO    <EventData>, <UserData>
// 8 YES   YES   YES   <Data Name=LogonID>0x37</Data> 
/////////////////////////////////////////////////////////

static void dump_element_txt(XML_ELEMENT *e, const char *parent_path)
{
    if (!e || !parent_path) return;

    //bool has_child = (e->first_child != NULL);
    bool has_text  = (e->text && e->text[0]);
    bool has_attr  = (e->attr_count > 0);

    char current_path[256];

    if (parent_path[0])
        snprintf(current_path, sizeof(current_path), "%s.%s", parent_path, e->name);
    else
        snprintf(current_path, sizeof(current_path), "%s", e->name);

    // CASE 1 and 5: no text, no attr
    if (!has_text && !has_attr)
    {
        printf("%s:\n", current_path);
    }

    // CASE 2 and 6: attr only
    if (!has_text && has_attr)
    {
        for (uint16_t i = 0; i < e->attr_count; i++)
        {
            XML_ATTRIBUTE *a = &e->attrs[i];
            if (a->value_type)
                printf("%s.%s: %s\n", current_path, a->name, a->value);
            else
                printf("%s.%s:\n", current_path, a->name);
        }
    }

    // CASE 3 and 7: text only
    if (has_text && !has_attr)
    {
        printf("%s: %s\n", current_path, e->text);
    }

    // CASE 4 and 8: text + attr
    if (has_text && has_attr)
    {
        bool norm = check_normaliz_needed(e->text);
        char *n_text = norm ? get_normalized_text(e->text) : NULL;
        const char *out = norm ? n_text : e->text;

        for (uint16_t i = 0; i < e->attr_count; i++)
        {
            XML_ATTRIBUTE *a = &e->attrs[i];
            if (a->value_type)
                printf("%s.%s: %s\n", current_path, a->value, out);
            else
                printf("%s: %s\n", current_path, out);
        }

        if (norm) free(n_text);
    }

    // recurse
    for (XML_ELEMENT *c = e->first_child; c; c = c->next_sibling)
        dump_element_txt(c, current_path);
}

void xml_dump_tree_txt(XML_TREE *tree)
{
    if (!tree || !tree->root) return;

    // Put seperator here
    char *recID = element_text(tree, "Event.System.EventRecordID");
    char *created = attribute_value(tree, "Event.System.TimeCreated", "SystemTime");
    char *evtID = element_text(tree, "Event.System.EventID");

    printf("##-------- %s\trecord_id=%s\tevent_id=%s --------\n", 
        created ? created : "(null)",
        recID ? recID : "(null)",
        evtID ? evtID : "(null)"
        );
 
    // dump as text
    dump_element_txt(tree->root, "");

    printf("##----------------------------------------------------------------------------\n");
}
