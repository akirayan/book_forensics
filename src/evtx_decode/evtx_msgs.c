#include "evtx_msgs.h"
#include <string.h>

typedef struct {
    const char *id;
    const char *msg;
} EVTX_MSG_MAP;

static const EVTX_MSG_MAP msg_table[] = {
    // Logon / Authentication (Common in Security.evtx)
    {"%%1963", "An account was successfully logged on."},
    {"%%1964", "An account failed to log on."},
    {"%%2048", "The logon attempt was made using explicit credentials."},
    
    // Elevation Levels (Seen in your %6 and %21 outputs)
    {"%%1936", "TokenElevationTypeDefault (1)"},
    {"%%1937", "TokenElevationTypeFull (2)"},
    {"%%1938", "TokenElevationTypeLimited (3)"},

    // Impersonation Levels
    {"%%1832", "Identification"},
    {"%%1833", "Impersonation"},
    {"%%1840", "Delegation"},
    {"%%1841", "Anonymous"},

    // Logon Types
    {"%%1842", "Interactive"},
    {"%%1843", "Network"},
    {"%%1844", "Batch"},
    {"%%1845", "Service"},
    {"%%1850", "RemoteInteractive"},

    // Boolean / Status Fallbacks
    {"%%1842", "Yes"}, // Note: IDs can overlap based on Provider; handle with care
    {"%%1843", "No"},
    {"%%1844", "System"},
    {"%%1845", "Not Available"},

    // Privileges (Common in Security Event 4672)
    {"%%1601", "SeAssignPrimaryTokenPrivilege"},
    {"%%1603", "SeTcbPrivilege"},
    {"%%1605", "SeSecurityPrivilege"},
    {"%%1608", "SeSystemtimePrivilege"},
    {"%%1612", "SeDebugPrivilege"},
    
    {NULL, NULL} // Sentinel
};

const char* resolve_evtx_message(const char* msg_id) {
    if (!msg_id || msg_id[0] != '%' || msg_id[1] != '%') {
        return msg_id;
    }

    for (int i = 0; msg_table[i].id != NULL; i++) {
        if (strcmp(msg_id, msg_table[i].id) == 0) {
            return msg_table[i].msg;
        }
    }

    return msg_id; // Return original if no match found
}














/********* SHELL FUNCTION ***************

lookup_messageId() {
    local v="$1"

    case "$v" in
        # --- Impersonation Levels ---
        %%1832) echo "Identification" ;;
        %%1833) echo "Impersonation" ;;
        %%1840) echo "Delegation" ;;
        %%1841) echo "Anonymous" ;;

        # --- Logon Types (Very common in 4624) ---
        %%1842) echo "Interactive" ;;           # Logon Type 2
        %%1843) echo "Network" ;;               # Logon Type 3
        %%1844) echo "Batch" ;;                 # Logon Type 4
        %%1845) echo "Service" ;;               # Logon Type 5
        %%1846) echo "Proxy" ;;                 # Logon Type 6
        %%1847) echo "Unlock" ;;                # Logon Type 7
        %%1848) echo "NetworkCleartext" ;;      # Logon Type 8
        %%1849) echo "NewCredentials" ;;        # Logon Type 9
        %%1850) echo "RemoteInteractive" ;;     # Logon Type 10
        %%1851) echo "CachedInteractive" ;;     # Logon Type 11

        # --- Elevation Types (Common in 4688) ---
        %%1936) echo "TokenElevationTypeDefault (1)" ;;
        %%1937) echo "TokenElevationTypeFull (2)" ;;
        %%1938) echo "TokenElevationTypeLimited (3)" ;;

        # --- Common Status/Boolean ---
        %%1842) echo "Yes" ;;                   # Context-dependent overlap
        %%1843) echo "No" ;;
        %%1844) echo "System" ;;
        %%1845) echo "NotAvailable" ;;
        %%1846) echo "Default" ;;

        # --- Access/Permission Results ---
        %%1537) echo "Success" ;;
        %%1538) echo "Failure" ;;
        %%1541) echo "Access Granted" ;;
        %%1542) echo "Access Denied" ;;

        *)  
            echo "$v"
            ;;  
    esac
}




**********************************/
