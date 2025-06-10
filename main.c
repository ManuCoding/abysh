#include <stdbool.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_CMD_LEN 4096
#define VERSION "0.1.0"

typedef struct {
	char** items;
	size_t cap;
	size_t len;
} StrArr;

#define DA_INIT_CAP 4
#define da_append(arr,item)                                         \
	do {                                                            \
		if((arr)->len>=(arr)->cap) {                                \
			if((arr)->cap) (arr)->cap*=2;                           \
			else (arr)->cap=DA_INIT_CAP;                            \
			void* items=malloc((arr)->cap*sizeof(item));            \
			if((arr)->items) {                                      \
				memcpy(items,(arr)->items,(arr)->len*sizeof(item)); \
				free((arr)->items);                                 \
			}                                                       \
			(arr)->items=items;                                     \
		}                                                           \
		(arr)->items[(arr)->len++]=item;                            \
	} while(0)

size_t trim(char** str) {
	size_t len=strnlen(*str,MAX_CMD_LEN);
	while(len && isspace((*str)[len-1])) {
		len--;
	}
	while(len && (*str)[0] && isspace((*str)[0])) {
		(*str)++;
		len--;
	}
	if(len<MAX_CMD_LEN) (*str)[len]='\0';
	return len;
}

void parse_args(StrArr* cmd,char* command) {
	size_t len=trim(&command);
	size_t tlen=0;
	for(size_t i=0; i<len; i++) {
		if(isspace(command[i])) {
			if(tlen) {
				command[i]='\0';
				da_append(cmd,command+i-tlen);
				tlen=0;
			}
		} else {
			tlen++;
		}
	}
	if(tlen) da_append(cmd,command+len-tlen);
}

void remove_dir(char* res,char* path) {
	char* shortpath=path;
	for(size_t i=0; path && path[i]; i++) {
		if(path[i]=='/') {
			shortpath=path+i+1;
		}
	}
	strcpy(res,shortpath);
}

void version(char* program,FILE* fd) {
	fprintf(fd,"%s (Abyss Shell) version %s\n",program,VERSION);
}

void help(char* program,FILE* fd) {
	version(program,fd);
	fprintf(fd,"\n");
	fprintf(fd,"List of builtin commands:\n");
	fprintf(fd,"    exit           Close the shell\n");
	fprintf(fd,"    hax            Attempt to close STDIN (bug testing)\n");
	fprintf(fd,"    cd directory   Change CWD to directory\n");
	fprintf(fd,"    version        Prints the version of the shell in a single line\n");
	fprintf(fd,"    help           Print this help\n");
}

int main(int argc,char** argv) {
	char* pname=argv[0];
	remove_dir(pname,pname);
	if(strlen(pname)==0 || argc<1) {
		pname="(abysh)";
		fprintf(stderr,"%s: Warning: weird environment\n",pname);
	}
	char command[MAX_CMD_LEN];
	char cwd[PATH_MAX];
	char promptpath[PATH_MAX];
	StrArr cmd={0};
	while(1) {
		getcwd(cwd,PATH_MAX);
		remove_dir(promptpath,cwd);
		if(promptpath[0]=='\0') strcpy(promptpath,cwd);
		printf("%s %s > ",pname,promptpath);
		command[0]='\0';
		if(!fgets(command,sizeof(command),stdin)) {
			return 0;
		}
		command[MAX_CMD_LEN-1]='\0';
		memset(&cmd,0,sizeof(cmd));
		parse_args(&cmd,command);
		if(cmd.len) {
			if(strcmp(cmd.items[0],"exit")==0) return 0;
			if(strcmp(cmd.items[0],"hax")==0) {
				printf("breaking your code >:)\n");
				fclose(stdin);
			}
			if(strcmp(cmd.items[0],"cd")==0) {
				char* newdir;
				if(cmd.len==1) {
					newdir=getenv("HOME");
				} else {
					newdir=cmd.items[1];
				}
				int res=chdir(newdir);
				if(res<0) {
					fprintf(stderr,"%s: cd %s: %s\n",pname,newdir,strerror(errno));
				}
				continue;
			}
			if(strcmp(cmd.items[0],"version")==0) {
				version(pname,stdout);
				continue;
			}
			if(strcmp(cmd.items[0],"help")==0) {
				help(pname,stdout);
				continue;
			}
			da_append(&cmd,NULL);
			pid_t pid=fork();
			if(pid==0) {
				int res=execvp(cmd.items[0],cmd.items);
				if(res<0) {
					fprintf(stderr,"Unknown command: %s\n",cmd.items[0]);
					return 127;
				}
				fprintf(stderr,"%s: internal error\n",pname);
				return 1;
			} else {
				int status;
				waitpid(pid,&status,0);
				printf("ret: %d\n",WEXITSTATUS(status));
			}
		}
	}
}
