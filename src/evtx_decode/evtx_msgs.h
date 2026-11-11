#ifndef EVTX_MSGS_H
#define EVTX_MSGS_H

/**
 * Resolves a Windows Message Resource ID (e.g., "%%1936") to its 
 * English description.
 * * @param msg_id The string starting with "%%"
 * @return The resolved string or the original msg_id if not found.
 */
const char* resolve_evtx_message(const char* msg_id);

#endif // EVTX_MSGS_H
