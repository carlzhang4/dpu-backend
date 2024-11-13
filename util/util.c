#include "util.h"

char* bytes2string(void* addr, int bytes){
	char *res = (char *)malloc(sizeof(char) * (bytes*4 + 1));
	unsigned char *byte_addr = (unsigned char *)addr;
	int offset=0;
	for(int i=0;i<bytes;i++){
		if((i+1)%8==0){
			sprintf(res + offset, "%02X", byte_addr[i]);
			offset+=2;
			sprintf(res + offset, " ");
			offset+=1;
		}else{
			sprintf(res + offset, "%02X_", byte_addr[i]);
			offset+=3;
		}

		if((i+1)%64==0){
			sprintf(res + offset, "\n");
			offset+=1;
		}
	}
	return res;
}