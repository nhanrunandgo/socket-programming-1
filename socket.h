/*
System test:
REQUEST_METADATA:_serverlist.svl
REQUEST_CHUNK:_serverlist.svl:0
REQUEST_CHUNK:_serverlist.svl:10
REQUEST_METADATA:filename
ssss
REQUEST_CHUNK:hi:
REQUEST_CHUNK:_serverlist.svl
*/

/** COMMANDS **/
#define REQUEST_METADATA "REQUEST_METADATA:"    // Structure: REQUEST_METADATA:filename
#define REPLY_METADATA "REPLY_METADATA:"        // Structure: REPLY_METADATA:filename:data_in_binary (struct Metadata)
#define REQUEST_CHUNK "REQUEST_CHUNK:"          // Structure: REQUEST_CHUNK:filename:chunk_id
#define REPLY_CHUNK "REPLY_CHUNK:"              // Structure: REPLY_CHUNK:filename:id:data
#define REPLY "REPLY:"                          // Structure: REPLY:ERROR

/** DEFINITIONS **/
#define SERVER_PORT 12345
#define BUFFER_SIZE 4096
#define CHUNK_SIZE 4096
#define MAX_FILE_LENGTH 256
#define FILES_DIR "files/"
