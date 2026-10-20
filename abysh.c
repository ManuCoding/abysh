#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define CMD_LEN 4096

int lastexit=0;
char *args[1+CMD_LEN/2]={0};

void parsecmd(char *curcmd,int *argsize) {
	*argsize=0;
	while(*curcmd) {
		while(*curcmd==' ') {
			*curcmd='\0';
			curcmd++;
		}
		if(!*curcmd) break;
		args[(*argsize)++]=curcmd;
		while(*curcmd && !isspace(*curcmd)) curcmd++;
	}
	args[*argsize]=NULL;
}

int main() {
	for(;;) {
		static char cmd[CMD_LEN];
		memset(cmd,0,sizeof(cmd));

		if(lastexit) printf("abysh [%d] > ",lastexit);
		else printf("abysh > ");
		fflush(stdout);

		if(!fgets(cmd,sizeof(cmd),stdin)) {
			return lastexit;
		}
		char *newline=strchr(cmd,'\n');
		if(newline) *newline='\0';

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
