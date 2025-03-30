#include "client.h"

uint64_t ntohll(uint64_t value) {
    return (((uint64_t)ntohl(value & 0xFFFFFFFF)) << 32) | ntohl(value >> 32); 
} 
   
uint64_t htonll(uint64_t value) {
    return (((uint64_t)htonl(value & 0xFFFFFFFF)) << 32) | htonl(value >> 32); 
} 

int main() {
    
}