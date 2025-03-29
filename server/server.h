/*
System test:
REQUEST_METADATA:_serverlist.svl
REQUEST_CHUNK:_serverlist.svl:0
REQUEST_CHUNK:_serverlist.svl:10
REQUEST_METADATA:filename
ssss
REQUEST_CHUNK:hi:
REQUEST_CHUNK:_serverlist.svl
REQUEST_CHUNK::
REQUEST_CHUNK:1MB.txt:5
REQUEST_METADATA:1MB.txt
*/

/** COMMANDS **/
#define REQUEST_METADATA "REQUEST_METADATA"    // Structure: REQUEST_METADATA:filename
#define REQUEST_CHUNK "REQUEST_CHUNK"          // Structure: REQUEST_CHUNK:filename:chunk_id

/*
Structure: REPLY:Seq#:Command
Including:
Server -> Client
- REPLY:0:CHUNK:filename:id:data
- REPLY:0:META:filename:data
- REPLY:0:ERROR:BAD REQUEST

Client -> Server
- REPLY:0:ACK
*/
#define REPLY "REPLY"
#define BAD_REQUEST "ERROR:BAD REQUEST\0"
#define INTERNAL_ERROR "ERROR:Internal server error"

/** DEFINITIONS **/
#define SERVER_PORT 12345
#define BUFFER_SIZE 4096
#define MAX_FILE 100
#define RELOAD_INTERVAL 5 // seconds
#define CHUNK_SIZE 1024
#define MAX_FILE_LENGTH 256
#define DOWNLOAD_DIR "files/"
#define DOWNLOAD_LIST "server_list.txt"


#define MAX_RETRIES 3
#define ACK_TIMEOUT 2000 // 2 seconds