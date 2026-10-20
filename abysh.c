#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int main() {
	for(;;) {
		static char cmd[4096];
		memset(cmd,0,sizeof(cmd));
		printf("abysh> ");
		fflush(stdout);
		if(!fgets(cmd,sizeof(cmd),stdin)) {
			return 0;
		}
		char *newline=strchr(cmd,'\n');
		if(newline) *newline='\0';
		if(strcmp(cmd,"exit")==0) {
			return 0;
		}
		pid_t child=fork();
		if(child) {
			int status;
			waitpid(child,&status,0);
			printf("ret: %d\n",WEXITSTATUS(status));
			continue;
		}
		char *args[]={"bash","-c",cmd,NULL};
		execvp("bash",args);
		return 127;
	}
	return 0;
}
