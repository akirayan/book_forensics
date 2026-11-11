#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "evtx_chunk.h"
#include "evtx_record.h"
#include "evtx_binxml.h"
#include "evtx_output.h"

#include "utf16le.h"
#include "timestamp.h"
#include "hex_dump.h"
#include "stack.h"


#define DUMP_HEX_BYTES 0


#pragma pack(push, 1)

// Token 0x0f: BINXML Fragment
typedef struct {
    uint8_t  token;             // should be 0x0f
    uint8_t  major_version;     // e.g., 0x01 for String, 0x04 for Hex32, etc.
    uint8_t  minor_version;     // e.g., 0x01 for String, 0x04 for Hex32, etc.
    uint8_t  flag;              // e.g., 0x01 for String, 0x04 for Hex32, etc.
} TOKEN_0F_FRAGMENT_HEADER;

// Token 0x01 / 0x41: Open Element
typedef struct {
    uint8_t  token;          // should be 0x01 or 0x41
    uint16_t dependency_id;  // Usually 0xFFFF
    uint32_t element_size;   // Total size of this element branch
    uint32_t name_offset;    // Offset to Name Descriptor
                             // name_offset maybe defined from here, or other position
                             // for 0x41: 4B as more_data_size 
} TOKEN_01_OPEN_ELEMENT_HEADER;

// Token 0x05: Value Token
typedef struct {
    uint8_t  token;          // should be 0x05
    uint8_t  value_type;     // e.g., 0x01 for String, 0x04 for Hex32, etc.
                             // followed by the actual data based on the type
} TOKEN_05_ATTRIBUTE_VALUE_HEADER;

// Token 0x06 / 0x46: Attribute Name
typedef struct {
    uint8_t  token;          // should be 0x06 or 0x46
    uint32_t name_offset;    // name offfset for Attribute Name
                             // NOTE for 0x46: NO MORE 4B as more_data_size 
} TOKEN_06_ATTRIBUTE_NAME_HEADER;

// Token 0x36: Attribute Name   (new token found by me on 2026.2.13)
typedef struct {
    uint8_t  token;          // should be 0x36
    uint8_t  unknown[4];     // Unknown/Dependency ID. Similar to the ff ff seen in 0x41, but 4 bytes. 
                             // This might be a more complex dependency identifier.
    uint32_t name_offset;    // name offfset for Attribute Name
} TOKEN_36_ATTRIBUTE_NAME_HEADER;

// Token 0x0d BinXmlTokenNormalSubstitution 
// Token 0x0e BinXmlTokenOptionallSubstitution
typedef struct {
    uint8_t  token;          // should be 0x0d or 0x0e
    uint16_t subs_id;        // Substitution identifier
    uint8_t value_type;      // eg. 01 as utf16le string 
} TOKEN_0E_SUBSTITUTION_HEADER;

// Token 0x0C: Template Instance Header
// the 33-byte structure including 24-byte of TEMPLATE_DEFINITION_HEADER
// now I use the 9-byte structure only
typedef struct {
    uint8_t  token;            // should be 0x0c
    uint8_t  unknown_val;      // unknown but usually seen as 0x01
    uint32_t template_id;      // the template ID as same as template_id
    uint32_t template_offset;  // offset to TEMPLATE_DEFINITION_HEADER, usually next 4 bytes
} TOKEN_0C_TEMPLATE_INSTANCE_HEADER;

// substitution value array
typedef struct {
    uint16_t size;          // raw value size
    uint8_t  type;          // EVTX value type (1 byte)
    uint8_t  dummy;         // always 0x00
    uint32_t value_offset;  // relative offset in chunk_buffer
} EVTX_VALUE_ITEM;

// table for get_value_by_index()
typedef struct {
    uint16_t count;
    EVTX_VALUE_ITEM *items;
} EVTX_VALUE_TABLE;

#pragma pack(pop)


// NOTE: this function is not used from 2026.2.11
const char* get_value_type_name(uint8_t value_type) 
{
    switch (value_type) {
        case 0x00: return "NullType";
        case 0x01: return "Utf16le";
        case 0x02: return "AnsiString";
        case 0x03: return "Int8Type";
        case 0x04: return "Uint8Type";
        case 0x05: return "Int16Type";
        case 0x06: return "Uint16Type";
        case 0x07: return "Int32Type";
        case 0x08: return "Uint32Type";
        case 0x09: return "Int64Type";
        case 0x0a: return "Uint64Type";
        case 0x0b: return "Real32Type";
        case 0x0c: return "Real64Type";
        case 0x0d: return "BoolType";
        case 0x0e: return "BinaryType";
        case 0x0f: return "GuidType";
        case 0x10: return "SizeTType";
        case 0x11: return "FileTime";
        case 0x12: return "SysTime";
        case 0x13: return "SidType";
        case 0x14: return "HexInt32";
        case 0x15: return "HexInt64";
        case 0x20: return "EvtHandle";
        case 0x21: return "BinXmlType";
        case 0x23: return "EvtXml";
        default: 
            if (value_type & 0x80) {
                 // if 080 is enabled, then it's array of above type
                 return "ArrayType";
            } else {
                return "UnknownType";
            }
    }
}




// if the name_offset just points to next byte, it means Online Name Offset, so we need to skip the bytes
// return 0 if defined at other location
static uint32_t get_inline_name_skip_bytes(uint8_t *chunk_buffer, uint32_t cursor_offset, uint32_t name_offset) 
{
    uint32_t skip_size = 0;

    if (cursor_offset == name_offset) { 
        // Name_Offset is just at the cursor, need to skip it
        EVTX_NAME_ENTRY_HEADER nh;
        memcpy(&nh, &chunk_buffer[cursor_offset], sizeof(nh));
        skip_size = sizeof(nh) + (nh.char_count * 2 + 2); // plus 2 NULLs
    }
    //printf("DEBUG: get_inline_name_skip_bytes() cursor=0x%x name_offset=0x%x skip_bytes=%d\n", 
    //                 cursor_offset, name_offset, skip_size);

    return skip_size;
}




static void print_evtx_filetime(uint64_t filetime, char *out_buf, size_t out_size) 
{

#define USE_7 

#ifdef USE_7

     return format_filetime(filetime, out_buf, out_size);

#else
    if (out_size < 32) return;

    // 100-nanosecond intervals between 1601 and 1970
    const uint64_t EPOCH_DIFF = 116444736000000000ULL;
    
    uint64_t unix_intervals = filetime - EPOCH_DIFF;
    time_t seconds = (time_t)(unix_intervals / 10000000ULL);
    uint32_t nanoseconds = (uint32_t)((unix_intervals % 10000000ULL) * 100);

    struct tm *utc_time = gmtime(&seconds);
    
    // Format: YYYY-MM-DDTHH:MM:SS.ssssssZ
    sprintf(out_buf, "%04d-%02d-%02dT%02d:%02d:%02d.%09uZ",
           utc_time->tm_year + 1900, utc_time->tm_mon + 1, utc_time->tm_mday,
           utc_time->tm_hour, utc_time->tm_min, utc_time->tm_sec, nanoseconds);
#endif
}

static void print_evtx_sid(uint8_t *sid_ptr, char *out_buf, size_t out_size) 
{
    if (!sid_ptr) return;
    if (out_size < 128) return;

    // Byte 0: Revision (S-n)
    uint8_t revision = sid_ptr[0];

    // Byte 1: Sub-Authority Count (How many 4-byte chunks follow)
    uint8_t sub_auth_count = sid_ptr[1];

    // Bytes 2-7: Identifier Authority (6 bytes, Big-Endian)
    // Most common is 00 00 00 00 00 05 (NT Authority)
    uint64_t authority = 0;
    for (int i = 0; i < 6; i++) {
        authority = (authority << 8) | sid_ptr[2 + i];
    }

    // Start printing the SID string
    sprintf(out_buf, "S-%u-%llu", revision, authority);

    // Bytes 8+: Sub-Authorities (4 bytes each, Little-Endian)
    uint32_t *sub_authorities = (uint32_t *)(sid_ptr + 8);
    for (int i = 0; i < sub_auth_count; i++) {
        char tmp_buf[20];
        sprintf(tmp_buf, "-%u", sub_authorities[i]);
        strcat(out_buf, tmp_buf);
    }
}

static void print_evtx_guid(uint8_t *guid_ptr, char *out_buf, size_t out_size) 
{
    if (!guid_ptr) return;
    if (out_size < 80) return;

    // GUID structure in memory:
    // Data1 (4B, LE), Data2 (2B, LE), Data3 (2B, LE), Data4 (8B, BE/Raw)
    
    uint32_t data1 = *(uint32_t *)&guid_ptr[0];
    uint16_t data2 = *(uint16_t *)&guid_ptr[4];
    uint16_t data3 = *(uint16_t *)&guid_ptr[6];

    sprintf(out_buf, "{%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
           data1, data2, data3,
           guid_ptr[8], guid_ptr[9],   // Data4 starts here
           guid_ptr[10], guid_ptr[11], 
           guid_ptr[12], guid_ptr[13], 
           guid_ptr[14], guid_ptr[15]);
}

// build the TABLE, set each member from chunk_buffer
static void create_value_table(EVTX_VALUE_TABLE *tbl, uint8_t *chunk_buffer, uint32_t value_table_offset)
{
    // value_array layout
    // 4B item_count    at value_table_offset
    // 2B size %0       at value_table_offset + 4 * 0
    // 1B type %0       at value_table_offset + 4 * 0 + 2
    // 1B dummy %0      at value_table_offset + 4 * 0 + 3  (always 0x00)
    // ....             repeat for all item_count
    // data %0          at value_table_offset + 4 + 4 * count
    // ...


    uint32_t count = *(uint32_t *)(chunk_buffer + value_table_offset);
    tbl->count = count;
    tbl->items = calloc(count, sizeof(EVTX_VALUE_ITEM));

    // the offset of data %0
    uint32_t value_offset = value_table_offset + sizeof(count) + count * 4; 

    for (uint32_t i = 0; i < count; i++) {
        uint16_t size = *(uint16_t *)(chunk_buffer + value_table_offset + sizeof(count) + i * 4);
        uint8_t  type = *(uint8_t  *)(chunk_buffer + value_table_offset + sizeof(count) + i * 4 + 2);
        // dummy byte at +3 is ignored

        tbl->items[i].size = size;
        tbl->items[i].type = type;
        tbl->items[i].value_offset = value_offset;

        value_offset += size;
    }
}

// free the allocated memory
static void delete_value_table(EVTX_VALUE_TABLE *tbl)
{
    free(tbl->items);
}





// the returned char *ptr should be freed by caller
// get the value by index
static char *get_value_by_index(EVTX_VALUE_TABLE *tbl, 
                                  uint8_t *chunk_buffer, 
                                  uint32_t index, 
                                  uint32_t output_mode, 
                                  XML_TREE *xtree)
{
    if (index >= tbl->count) return NULL;

    // NOTE: the output buffer, allocated in this function, but need to be freed by caller
    char *out_buf = NULL;   

    EVTX_VALUE_ITEM *val_item = &tbl->items[index];
    uint16_t size = val_item->size;
    uint8_t type = val_item->type;
    uint8_t *data_ptr = chunk_buffer + val_item->value_offset;

    // Handle empty values 
    if (size == 0 && type != 0x00) {
        out_buf = (char *)malloc(80);
        sprintf(out_buf, "[Empty]");
        return out_buf;
    }

    switch (type) {
        case 0x00: // NullType
            out_buf = (char *)malloc(80);
            sprintf(out_buf, "(NullType)");
            break;

        case 0x01: // StringType (Unicode UTF-16LE)
            // We pass the size in bytes to our helper
            out_buf = get_utf16le_string(size/2, (uint16_t *)data_ptr);
            break;

        case 0x02: // AnsiStringType
            out_buf = (char *)malloc(size + 2);
            sprintf(out_buf, "%.*s", size, (char *)data_ptr);
            break;

        case 0x03: // Int8Type 
            out_buf = (char *)malloc(80);
            if (size == 1) sprintf(out_buf, "%d", *data_ptr);
            else if (size == 4) sprintf(out_buf, "%d", *(int32_t *)data_ptr);
            break;

        case 0x04: // libevtx doc: Uint8Type (gemini said UInt32)
            out_buf = (char *)malloc(80);
            if (size == 1) sprintf(out_buf, "%u", *data_ptr);
            else if (size == 4) sprintf(out_buf, "%u", *(uint32_t *)data_ptr);
            break;

       case 0x05: // Int16Type
           out_buf = (char *)malloc(80);
           out_buf[0] = 0x00;
           snprintf(out_buf, 80, "[type 0x05 size=%u]", size);
           if (size == 2) sprintf(out_buf, "%d", *(int16_t *)data_ptr);
           break;

       case 0x06: // Uint16Type
           out_buf = (char *)malloc(80);
           out_buf[0] = 0x00;
           snprintf(out_buf, 80, "[type 0x06 size=%u]", size);
           if (size == 2) sprintf(out_buf, "%u", *(uint16_t *)data_ptr);
           break;

        case 0x07: // Int32Type 
            out_buf = (char *)malloc(80);
            if (size == 4) sprintf(out_buf, "%d", *(uint32_t *)data_ptr);
            else if (size == 8) sprintf(out_buf, "%lld", *(int64_t *)data_ptr);
            break;

        case 0x08: // Uint32Type 
            out_buf = (char *)malloc(80);
            if (size == 4) sprintf(out_buf, "%u", *(uint32_t *)data_ptr);
            else if (size == 8) sprintf(out_buf, "%llu", *(uint64_t *)data_ptr);
            break;

        case 0x09: // Int64Type
            out_buf = (char *)malloc(80);
            sprintf(out_buf, "%lld", *(int64_t *)data_ptr);
            break;

        case 0x0A: // Uint64Type
            out_buf = (char *)malloc(80);
            sprintf(out_buf, "%llu", *(uint64_t *)data_ptr);
            break;

        case 0x0D: // Boolean: 32-bit int, 0x00 (false) or 0x01 (false)
            out_buf = (char *)malloc(80);
            sprintf(out_buf, "%s", *(int32_t *)data_ptr ? "true" : "false");
            break;


        case 0x0E: // BinaryType
        {
            // size is already in val_item->size, no extra size bytes in data
            out_buf = (char *)malloc(size * 2 + 1);
            if (out_buf) {
                char *ptr = out_buf;
                for (int j = 0; j < size; j++) {
                    sprintf(ptr, "%02X", data_ptr[j]);
                    ptr += 2;
                }
                *ptr = '\0';
            }
            break;
        }

        case 0x0F: // GuidType
            out_buf = (char *)malloc(80);
            print_evtx_guid(data_ptr, out_buf, 80);
            break;

        case 0x11: // FileTimeType
            out_buf = (char *)malloc(80);
            print_evtx_filetime(*(uint64_t *)data_ptr, out_buf, 80);
            break;

        case 0x13: // SidType (0x13 or 0x1C depending on version)
            out_buf = (char *)malloc(128);
            print_evtx_sid(data_ptr, out_buf, 128);
            break;

        case 0x14: // HexInt32Type
            out_buf = (char *)malloc(80);
            sprintf(out_buf, "0x%x", *(uint32_t *)data_ptr);
            break;

        case 0x15: // HexInt64Type
            out_buf = (char *)malloc(80);
            sprintf(out_buf, "0x%llx", *(uint64_t *)data_ptr);
            break;

        case 0x21: // BinXmlType call decode_binxml to decode it
            if (CHECK_OUTMODE(output_mode, OUT_DEBUG)) {
                printf("[Embedded BinXML Area - %d bytes]", size);
                printf("\nDEBUG: called from get_value_by_index()\t");
            }
            decode_binxml(chunk_buffer, val_item->value_offset, val_item->size, output_mode, xtree); 
            break;


        case 0x81: // Array of Utf16le (NULL-terminated UTF-16LE strings)
        {
            char *result = strdup("");
            uint8_t *ptr = data_ptr;
            uint8_t *end = data_ptr + size;
            int first = 1;

            while (ptr < end) {
                // count chars until NULL terminator (0x0000)
                int char_count = 0;
                while ((ptr + char_count * 2 + 1) < end &&
                       *(uint16_t *)(ptr + char_count * 2) != 0x0000) {
                    char_count++;
                }

                if (char_count > 0) {
                    char *elem = get_utf16le_string(char_count, (uint16_t *)ptr);
                    size_t new_len = strlen(result) + strlen(elem) + 3;
                    result = realloc(result, new_len);
                    if (!first) strcat(result, ", ");
                    strcat(result, elem);
                    free(elem);
                    first = 0;
                }

                ptr += (char_count + 1) * 2; // skip string + NULL terminator
            }

            out_buf = result;
            break;
        }

        // NOTE: it seems this type is not included in any output.
        //       since the XML output is as same as EventViewer, we just ignore this part.
        case 0x84: // Array of Uint8Type (0x80 | 0x04)
        {
            // Each element is 1 byte (uint8) per libevtx spec.
            // However, in practice some EVTX records store UInt32 elements
            // (4 bytes each) under the same type tag.
            // Heuristic: if size is divisible by 4 AND size >= 8,
            // treat as UInt32 array; otherwise treat as UInt8 array.
            // Both variants are output as comma-separated decimal values.

            int elem_count;
            int elem_size;

            if (size >= 4 && (size % 4) == 0) {
                // Treat as array of UInt32 (4 bytes per element)
                elem_size  = 4;
                elem_count = size / 4;
            } else {
                // Treat as array of UInt8 (1 byte per element)
                elem_size  = 1;
                elem_count = size;
            }

            // Allocate: UInt32 max 10 digits + ", " (2) per element + NUL
            out_buf = (char *)malloc(elem_count * 13 + 4);
            if (out_buf) {
                char *wp = out_buf;
                wp[0] = '\0';
                for (int j = 0; j < elem_count; j++) {
                    if (j > 0) {
                        strcpy(wp, ", ");
                        wp += 2;
                    }
                    if (elem_size == 4) {
                        wp += sprintf(wp, "%u", *(uint32_t *)(data_ptr + j * 4));
                    } else {
                        wp += sprintf(wp, "%u", data_ptr[j]);
                    }
                }
                //printf("DEBUG: type=0x84 out_buf=%s\n", out_buf);
            }
            break;
        }

       default:
           out_buf = (char *)malloc(80); // to let caller free the buffer
           out_buf[0] = 0x00; 
           fprintf(stderr, "ERROR: get_value_by_index() [Unknown Type 0x%02x, size %d]", type, size);
           break;
    }

    // rerun the out_buf, which should be freed by caller
    return out_buf;
}




static void decode_template_with_values(
                   uint8_t *chunk_buffer,        /* the 64KB chunk in memory */
                   uint32_t binxml_offset,       /* start position of binxml, related to chunk_buffer */
                   uint32_t binxml_size,         /* the size of buffer =  record_size - 24 - 4  */       
                   EVTX_VALUE_TABLE *tbl_ptr,    /* value table of this binxml */
                   uint32_t output_mode,         /* CSV or DEBUG etc */
                   XML_TREE *xtree)              /* the tree */
{
    uint32_t i = binxml_offset;  // the starting point of cursor in buffer
    uint32_t binxml_limit = binxml_offset + binxml_size;  // the hard limit of cursor in buffer

    STACK *stack = stack_new(); // to hold element names

    if (CHECK_OUTMODE(output_mode, OUT_DEBUG)) {
        printf(" decode_template_with_values() offset=0x%08" PRIx32 " size=%" PRIu32 "\n", 
                binxml_offset, binxml_size);
        hex_dump_bytes(&chunk_buffer[binxml_offset], binxml_size);
    }

    // decode the template binxml per byte and insert value from VALUE_TABLE if needed
    while (i < binxml_limit ) {
        uint8_t raw_token = chunk_buffer[i];

        //printf("\nDEBUG: cursor=0x%x\traw_token=0x%02x\n", i, raw_token);
        
        switch (raw_token) {

            case 0x0f: // BinXmlFragmentHeaderToken
            {
                TOKEN_0F_FRAGMENT_HEADER fh;
                memcpy(&fh, &chunk_buffer[i], sizeof(fh));
                i += sizeof(fh); // [Token 1B] [MajorVersion 1B] [MinorVersion 1B] [Flags 1B]
                break;
            }

            case 0x01: // BinXmlTokenOpenStartElement
            case 0x41: // BinXmlTokenOpenStartElement | BinXmlTokenMoreData
            {
                TOKEN_01_OPEN_ELEMENT_HEADER open_el;
                memcpy(&open_el, &chunk_buffer[i], sizeof(open_el));
                i += sizeof(open_el); // skip (Token + DependencyID + DataSize + NameOffset)
            
                char *name_buf = get_name_from_offset(chunk_buffer, open_el.name_offset);
                stack_push(stack, name_buf);
                xml_push_element(xtree, name_buf);
                if (CHECK_OUTMODE(output_mode, OUT_DEBUG)) {
                    printf("<%s", name_buf);
                }
                free(name_buf);

                // if the name_offset is defined at here, skip the whole name buffer
                i += get_inline_name_skip_bytes(chunk_buffer, i, open_el.name_offset);
            
                if (raw_token & 0x40) { 
                    // if token=0x41, there are 4 bytes as attr_list_size
                    // uint32_t attr_list_size = *(uint32_t*)&chunk_buffer[i]; 
                    i += 4; 
                }
            
                break;
            }
            
            case 0x06: // BinXmlTokenAttribute
            case 0x46: // BinXmlTokenAttribute | BinXmlTokenMoreData
            {
                TOKEN_06_ATTRIBUTE_NAME_HEADER attr;
                memcpy(&attr, &chunk_buffer[i], sizeof(attr));
                i += sizeof(attr); // token + 4B 
            
                char *name_buf = get_name_from_offset(chunk_buffer, attr.name_offset);
                xml_begin_attribute(xtree, name_buf);
                if (CHECK_OUTMODE(output_mode, OUT_DEBUG)) {
                    printf(" %s=", name_buf);
                }
                free(name_buf);
            
                // if the name_offset is defined at here, skip the whole name buffer
                i += get_inline_name_skip_bytes(chunk_buffer, i, attr.name_offset);
            
                // NOTE for 0x46
                // there is NO 4 bytes as more_data_size

                break;
            }


            case 0x36: // new token seems for attribute name
            {
                TOKEN_36_ATTRIBUTE_NAME_HEADER attr;
                memcpy(&attr, &chunk_buffer[i], sizeof(attr));
                i += sizeof(attr); // token + 4B unkown + 4B name_offset 
            
                char *name_buf = get_name_from_offset(chunk_buffer, attr.name_offset);
                xml_begin_attribute(xtree, name_buf);
                if (CHECK_OUTMODE(output_mode, OUT_DEBUG)) {
                    printf(" %s=", name_buf);
                }
                free(name_buf);
            
                // if the name_offset is defined at here, skip the whole name buffer
                i += get_inline_name_skip_bytes(chunk_buffer, i, attr.name_offset);
            
                break;
            }


            case 0x05:
            case 0x45:
            {
                TOKEN_05_ATTRIBUTE_VALUE_HEADER vh;
                memcpy(&vh, &chunk_buffer[i], sizeof(vh));
                i += sizeof(vh);
            
                uint8_t v_type = vh.value_type;
            
                char *v_text = NULL;
            
                switch (v_type)
                {
                    case 0x01:
                    {
                        uint16_t char_count = *(uint16_t*)&chunk_buffer[i];
                        i += 2;  // skip char_count
            
                        v_text = get_utf16le_string(char_count, (uint16_t*)&chunk_buffer[i]);
                        i += char_count * 2;  // skip utf16le string and 2 NULLs
                        break;
                    }
            
                    case 0x00:
                        v_text = strdup("(null)");
                        break;
            
                    default:
                        v_text = strdup("");
                        break;
                }
            
                if (xml_current_attribute(xtree)) { // attribute value
                    xml_end_attribute(xtree, v_text, v_type);
                } else {                            // element text
                    xml_set_element(xml_current_element(xtree), v_text, v_type);
                }
                if (CHECK_OUTMODE(output_mode, OUT_DEBUG)) {
                    printf("%s", v_text);
                }
                
                free(v_text);
                break;
            }


            case 0x0d: // BinXmlTokenNormalSubstitution
            case 0x0e: // BinXmlTokenOptionalSubstitution
            {
                TOKEN_0E_SUBSTITUTION_HEADER sh;
                memcpy(&sh, &chunk_buffer[i], sizeof(sh));
                i += sizeof(sh); // token + 2B subID + 1B type

                //printf("DEBUG: subs_id=%%%d\n", sh.subs_id);
                EVTX_VALUE_ITEM *val_item = &tbl_ptr->items[sh.subs_id];
                char *value = get_value_by_index(tbl_ptr, chunk_buffer, sh.subs_id, output_mode, xtree);

                if (value) {
                    uint8_t real_type = val_item->type;

                    if (xml_current_attribute(xtree)) {
                        xml_end_attribute(xtree, value, real_type);
                    } else {
                        xml_set_element(xml_current_element(xtree), value, real_type);
                    }
                    if (CHECK_OUTMODE(output_mode, OUT_DEBUG)) {
                        printf("%s", value);
                    }

                    free(value);
                }

                // how to handle array type?

                break;
            }
            

            case 0x08: // BinXmlTokenCharRef
            case 0x48: // BinXmlTokenCharRef
            case 0x09: // BinXmlTokenEntityRef
            case 0x49: // BinXmlTokenEntityRef
            case 0x07: // BinXmlTokenCDATASection
            case 0x47: // BinXmlTokenCDATASection
            case 0x0a: // BinXmlTokenPITarget
            case 0x0b: // BinXmlTokenCDATASection
                fprintf(stderr, "WARNING: token=0x%02x at 0x%x\n", raw_token, i);
                i += 1; // token
                break;

            case 0x0c: // BinXmlTokenTemplateInstance
                // this should never appear in template_binxml
                fprintf(stderr, "ERROR: token=0C should not appear here 0x%x, something WRONG?\n", i);
                i += 1;
                return;
                break;
            
            case 0x02: // BinXmlTokenCloseStartElementTag
                i += 1;
                if (CHECK_OUTMODE(output_mode, OUT_DEBUG)) {
                    printf(">");
                }
                break;

            case 0x03: // BinXmlTokenCloseEmptyElementTag
                i += 1;
                stack_pop(stack);  // this is an empty element, we need to pop it from stack
                xml_pop_element(xtree);
                if (CHECK_OUTMODE(output_mode, OUT_DEBUG)) {
                    printf("/>\n");
                }
                break;

            case 0x04: // BinXmlTokenEndElementTag
                i += 1;
                xml_pop_element(xtree);
                if (CHECK_OUTMODE(output_mode, OUT_DEBUG)) {
                    printf("</%s>\n", stack_pop(stack));
                }
                break;

            case 0x00: // BinXmlTokenEOF  EOF or Padding, just skip it to next byte
                i += 1;
                break;

            default:
                fprintf(stderr, "WARNING: token=0x%02x at 0x%x NOT PROCESSED\n", raw_token, i);
                i += 1; 
                break;
        }
    }
}





void decode_binxml(uint8_t *chunk_buffer,        /* the 64KB chunk in memory */
                   uint32_t binxml_offset,       /* start position of binxml, related to chunk_buffer */
                   uint32_t binxml_size,         /* the size of buffer =  record_size - 24 - 4  */       
                   uint32_t output_mode,         /* CSV or DEBUG etc */
                   XML_TREE *xtree)              /* XML TREE of output */
{
    // binxml can be splitted into 3 parts:
    //     {template-ID-Offset} {optional: definition} {instance data}
    //     part1: leading 0f token and 0C token, specify template ID and offset 
    //     part2: (optional) the definition of template, but it maybe located in other records
    //     part3: instance data array, we call this as value_table, will be substituted for %# of 0E token
    // decode the binxml by following steps
    //     1) find the tempalate offset from part1, get the offset of template_binxml
    //     2) find the starting position of part3, create value_table
    //     3) create the instance by mergring template with values


    uint32_t template_binxml_offset = 0x00;
    uint32_t template_binxml_size = 0x00;
    uint32_t value_table_offset = 0x00;

    EVTX_VALUE_TABLE value_table;

    if (CHECK_OUTMODE(output_mode, OUT_DEBUG)) {
        printf("decode_binxml() offset=0x%08" PRIx32 "\tsize=%" PRIu32 "\n", binxml_offset, binxml_size);
    }

    // first find template specified by token 0C, usuaaly in the very begining
    for (uint32_t i = binxml_offset; i < binxml_offset + 10; i++) { 
        if ((chunk_buffer[i] == 0x0c) && (chunk_buffer[i + 1] == 0x01)) {// found it.
            TOKEN_0C_TEMPLATE_INSTANCE_HEADER token_h;
            memcpy(&token_h, &chunk_buffer[i], sizeof(token_h));

            EVTX_TEMPLATE_DEFINITION_HEADER th;
            memcpy(&th, &chunk_buffer[token_h.template_offset], sizeof(th));
                  // if needed, make sure this is a real template offset by checking 
                  //    if token_h.template_offset existing in index table (0x180 to 0x1ff)

            template_binxml_offset = token_h.template_offset + sizeof(th);
            template_binxml_size = th.data_size;
            value_table_offset = i + sizeof(token_h); 

            if (CHECK_OUTMODE(output_mode, OUT_DEBUG)) {
                printf("DEBUG: Found 0C 01 at 0x%08" PRIx32 "", i);
                printf("\ttemplate_id=0x%08" PRIx32 "\tbinxml_offset=0x%08" PRIx32 "\tsize=%" PRIu32 "B\n", 
                         th.template_id, template_binxml_offset, template_binxml_size);
            }

            break; // no more need to loop since already found it.
         }
      }
      if (!template_binxml_offset) {
          fprintf(stderr, "ERROR: no 0C token found after 0x%x\n", binxml_offset); // should never happen
          return;
      }

      // adjust the value_table_offset if part2 existing
      if ((binxml_offset < template_binxml_offset) && 
                 (template_binxml_offset < binxml_offset + binxml_size)) {
           // template binxml is within the original binxml, i.e. size of part2
           value_table_offset += template_binxml_size + sizeof(EVTX_TEMPLATE_DEFINITION_HEADER);
      }

      // build the value_table
      create_value_table(&value_table, chunk_buffer, value_table_offset);

      // now, merge the template_binxml with value data
      decode_template_with_values(chunk_buffer, 
              template_binxml_offset, template_binxml_size, 
              &value_table, output_mode, xtree);

      // free memory
      delete_value_table(&value_table);
}
