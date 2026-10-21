#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define CMD_LEN 4096

int lastexit=0;
char *args[1+CMD_LEN/2]={0};

typedef struct {
	size_t len;
	char *s;
} S;

// TODO implement `replacerange` function
//      this will help finalize the concept of "dynamic string"

void parsecmd(S curcmd,int *argsize) {
	*argsize=0;
	while(curcmd.len) {
		while(curcmd.len && *curcmd.s==' ') {
			*curcmd.s='\0';
			curcmd.s++;
			curcmd.len--;
		}
		if(!curcmd.len) break;
		args[(*argsize)++]=curcmd.s;
		while(curcmd.len && !isspace(*curcmd.s)) {
			curcmd.s++;
			curcmd.len--;
		}
	}
	args[*argsize]=NULL;
}

int main() {
	for(;;) {
		static char cmdstr[CMD_LEN];
		memset(cmdstr,0,sizeof(cmdstr));

		if(lastexit) printf("abysh [%d] > ",lastexit);
		else printf("abysh > ");
		fflush(stdout);

		if(!fgets(cmdstr,sizeof(cmdstr),stdin)) {
			return lastexit;
		}
		char *newline=strchr(cmdstr,'\n');
		if(newline) *newline='\0';

		S cmd={ .len=strlen(cmdstr), .s=cmdstr };

		int argsize=0;
		parsecmd(cmd,&argsize);
		if(argsize==0) continue;

		if(strcmp(args[0],"exit")==0) {
			if(argsize>1) return atoi(args[1]);
			return lastexit;
		}

		pid_t child=fork();
		if(child) {
			int status;
			waitpid(child,&status,0);
			lastexit=WEXITSTATUS(status);
			continue;
		}

		execvp(args[0],args);
		return 127;
	}
	return lastexit;
}
