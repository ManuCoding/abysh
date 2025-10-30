#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define MAX_CMD_LEN 4096
#define VERSION "0.3.0"

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

typedef struct termios Termios;
Termios initial_state={0};
int keys_fd=0;
size_t term_width=80;
char* pname;
// 8MB should be enough for everyone
#define ENVPOOL_SIZE (8*1024*1024)
char envpool[ENVPOOL_SIZE];
char retbuf[1024];

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

bool parse_string(char* command,size_t* len,size_t* idx) {
	if(*idx>=*len) return false;
	if(command[*idx]!='"') return false;
	size_t startidx=*idx;
	for((*idx)++; *idx<*len; (*idx)++) {
		if(command[*idx]=='"') {
			for(size_t i=startidx; i+1<*idx; i++) {
				command[i]=command[i+1];
			}
			for(size_t i=(*idx)-1; i+2<*len; i++) {
				command[i]=command[i+2];
			}
			*idx-=2;
			--*len;
			command[--*len]='\0';
			return true;
		}
		if(command[*idx]=='\\') {
			if(*idx+1<*len) {
				for(size_t i=*idx; i+1<*len; i++) {
					command[i]=command[i+1];
				}
				command[--*len]='\0';
				continue;
			}
			return false;
		}
	}
	return false;
}

bool parse_args(StrArr* cmd,StrArr* tmpvars,char* command) {
	size_t len=trim(&command);
	size_t tlen=0;
	for(size_t i=0; i<len; i++) {
		if(command[i]=='=') {
			if(cmd->len>0) {
				tlen++;
				continue;
			}
			if(tlen) {
				for(; i<len && !isspace(command[i]); i++) tlen++;
				command[i]='\0';
				da_append(tmpvars,command+i-tlen);
				tlen=0;
				continue;
			}
		}
		if(isspace(command[i])) {
			if(tlen) {
				command[i]='\0';
				da_append(cmd,command+i-tlen);
				tlen=0;
			}
		} else {
			tlen++;
		}
		if(command[i]=='"') {
			size_t idx=i;
			if(!parse_string(command,&len,&i)) {
				fprintf(stderr,"%s: unexpected EOF while looking for matching '\"'\n",pname);
				return false;
			}
			tlen+=i-idx;
		}
	}
	if(tlen) da_append(cmd,command+len-tlen);
	return true;
}

void expand_path(StrArr cmd,char* cwd,char* pathenv,char* pathbuf) {
	if(cmd.len) {
		size_t len=strlen(cmd.items[0]);
		size_t tlen=0;
		for(size_t i=0; i<len; i++) {
			if(cmd.items[0][i]=='/') {
				if(i==0) { // Absolute path
					snprintf(pathbuf,PATH_MAX,"%s",cmd.items[0]);
					pathbuf[PATH_MAX-1]='\0';
					return;
				}
				snprintf(pathbuf,PATH_MAX,"%s/%s",cwd,cmd.items[0]);
				pathbuf[PATH_MAX-1]='\0';
				return;
			}
		}
		len=strlen(pathenv);
		for(size_t i=0; i<=len; i++) {
			if(pathenv[i]==':' || i==len) {
				snprintf(pathbuf,PATH_MAX,"%.*s/%s",(int)tlen,pathenv+i-tlen,cmd.items[0]);
				pathbuf[PATH_MAX-1]='\0';
				if(access(pathbuf,X_OK)==0) return;
				tlen=0;
				continue;
			}
			tlen++;
		}
		pathbuf[0]='\0';
	}
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

void getsize(int _sig) {
	(void)_sig;
	struct winsize win;
	ioctl(0,TIOCGWINSZ,&win);
	term_width=win.ws_col;
}

void readline(char* prompt,char* command,StrArr history) {
	size_t idx=0;
	size_t curlen=0;
	size_t hist_idx=history.len;
	bool edited=false;
	getsize(0);
	size_t prompt_len=strlen(prompt)%term_width;
	tcgetattr(keys_fd,&initial_state);
	Termios raw=initial_state;
	raw.c_lflag&=~(ISIG|ICANON|ECHO);
	raw.c_cc[VMIN]=1;
	raw.c_cc[VTIME]=0;
	printf("%s",prompt);
	command[0]='\0';
	tcsetattr(keys_fd,TCSAFLUSH,&raw);
	unsigned char ch=0;
	while(ch!='\n') {
		ch=getchar();
got_char:
		if(!ch) break;
		switch(ch) {
			case '\x1b':
parse_esc:
				ch=getchar();
				switch(ch) {
					case '[':
						goto parse_esc;
					case 'A':
prev_hist:
						if(hist_idx>0) {
							if(curlen+prompt_len>term_width) {
								printf("\x1b[%zuB",(curlen-idx+prompt_len)/term_width);
								for(size_t i=0; i<curlen+prompt_len; i+=term_width) printf("\r\x1b[K\x1b[A");
							}
							if(edited) {
								// TODO search backwards through history for a match of current command
							}
							if(history.items[--hist_idx]) {
								memcpy(command,history.items[hist_idx],MAX_CMD_LEN);
								curlen=strlen(command);
								idx=curlen;
							}
							edited=false;
							printf("\r\x1b[K%s%*s",prompt,(int)curlen,command);
						}
						break;
					case 'B':
next_hist:
						if(hist_idx<history.len) {
							if(curlen+prompt_len>term_width) {
								printf("\x1b[%zuB",(curlen-idx+prompt_len)/term_width);
								for(size_t i=0; i<curlen+prompt_len; i+=term_width) printf("\r\x1b[K\x1b[A");
							}
							if(edited) {
								// TODO search backwards through history for a match of current command
							}
							if(++hist_idx<history.len && history.items[hist_idx]) {
								memcpy(command,history.items[hist_idx],MAX_CMD_LEN);
								curlen=strlen(command);
								idx=curlen;
							} else {
								curlen=0;
								idx=0;
								command[0]='\0';
							}
							edited=false;
							printf("\r\x1b[K%s%*s",prompt,(int)curlen,command);
						}
						break;
					case 'C':
move_right:
						if(idx<curlen) {
							printf("%c",command[idx]);
							idx++;
						}
						break;
					case 'D':
move_left:
						if(idx>0) {
							printf("\b");
							idx--;
						}
						break;
					case '3': // Delete
						ch=getchar();
						if(ch!='~') goto got_char;
delete_char:
						if(idx<curlen) {
							for(size_t i=idx; i+1<curlen; i++) {
								command[i]=command[i+1];
							}
							curlen--;
							printf("\x1b""7%.*s \x1b""8",(int)(curlen-idx),command+idx);
						}
						break;
					default:
						goto got_char;
				}
				break;
			case 'A'-'@':
				if(idx+prompt_len>term_width) printf("\x1b[%zuA",(idx+prompt_len)/term_width);
				printf("\r\x1b[%zuC",prompt_len);
				idx=0;
				break;
			case 'E'-'@':
				if(curlen+prompt_len>term_width) printf("\x1b[%zuB",(curlen-idx+prompt_len)/term_width);
				printf("\r\x1b[%zuC",(curlen+prompt_len)%term_width);
				idx=curlen;
				break;
			case 'D'-'@':
				if(curlen>0) goto delete_char;
				// FIXME very hacky way of quitting :)
				sprintf(command,"exit");
				curlen=4;
				ch='\n';
				break;
			case 'C'-'@':
				printf("\r%*c\r%s",(int)(prompt_len+curlen),' ',prompt);
				curlen=0;
				break;
			case 'F'-'@':
				goto move_right;
			case 'B'-'@':
				goto move_left;
			case 'H'-'@':
			case 127: // Backspace
				if(idx>0) {
					for(size_t i=idx-1; i<curlen && i+1<MAX_CMD_LEN; i++) {
						command[i]=command[i+1];
					}
					curlen--;
					printf("\x1b[D\x1b""7%.*s \x1b""8",(int)(curlen-idx+1),command+idx-1);
					idx--;
				}
				break;
			case 'L'-'@':
				printf("\x1b[H\x1b[2J%s%*s",prompt,(int)curlen,command);
				for(size_t i=curlen; i>idx; i--) {
					printf("\b");
				}
				break;
			case 'N'-'@':
				goto next_hist;
				break;
			case 'P'-'@':
				goto prev_hist;
				break;
			default:
				if(ch<' ') continue;
				if(idx+1>=MAX_CMD_LEN) continue;
				if(idx<curlen) {
					printf(" %.*s\b",(int)(curlen-idx),command+idx);
					for(size_t i=curlen; i>idx; i--) {
						command[i]=command[i-1];
						printf("\b");
					}
				}
				curlen++;
				if(curlen>=MAX_CMD_LEN) curlen=MAX_CMD_LEN-1;
				printf("%c",ch);
				fflush(stdout);
				command[idx]=ch;
				edited=true;
				idx++;
		}
		if(curlen<MAX_CMD_LEN) command[curlen]='\0';
		else curlen--;
	}
	for(size_t i=(prompt_len+curlen-idx)/term_width+1; i>0; i--) printf("\r\n");
	tcsetattr(keys_fd,TCSAFLUSH,&initial_state);
	command[MAX_CMD_LEN-1]='\0';
}

void add_history(char* command,StrArr* history) {
	if(history->len>0 && strcmp(command,history->items[history->len-1])==0) return;
	char* copy=malloc(MAX_CMD_LEN);
	memcpy(copy,command,strlen(command));
	da_append(history,copy);
}

char* envget(StrArr env,char* var) {
	if(var==NULL) return NULL;
	size_t varlen=strlen(var);
	for(size_t i=0; i<env.len; i++) {
		if(strncmp(env.items[i],var,varlen)==0) {
			if(env.items[i][varlen]=='=') return env.items[i]+varlen+1;
		}
	}
	return NULL;
}

void envedit(StrArr* env,char* var,char* val) {
	if(var==NULL) return;
	size_t varlen=strlen(var);
	for(size_t i=0; i<env->len; i++) {
		if(strncmp(env->items[i],var,varlen)==0 && env->items[i][varlen]=='=') {
			char* varat=env->items[i]-1;
			env->items[i]=env->items[env->len-1];
			env->len--;
			if(*(uint8_t*)varat==255) {
				size_t curlen=strlen(varat+3)+3;
				for(size_t i=0; i<curlen; i+=256) {
					*(char*)(varat+i)=0;
				}
			}
			*varat=0;
			break;
		}
	}
	if(val==NULL || val[0]=='\0') return;
	size_t totallen=varlen+strlen(val)+2;
	if(totallen<255) {
		size_t idx=0;
		while(idx<ENVPOOL_SIZE && envpool[idx]) idx+=256;
		if(idx>=ENVPOOL_SIZE) {
			fprintf(stderr,"%s: could not set '%s': out of memory\n",pname,var);
			return;
		}
		envpool[idx]=totallen;
		sprintf(envpool+idx+1,"%s=%s",var,val);
		da_append(env,&envpool[idx+1]);
		return;
	}
	size_t idx=0;
	size_t idx2=0;
	if(totallen%256==255) totallen+=256;
find_big_slot:
	while(idx<ENVPOOL_SIZE && envpool[idx]) idx+=256;
	for(idx2=idx; idx2<ENVPOOL_SIZE && idx2-idx<=totallen; idx2+=256) {
		if(envpool[idx2]) goto find_big_slot;
	}
	if(idx2>=ENVPOOL_SIZE) {
		fprintf(stderr,"%s: could not set '%s': out of memory\n",pname,var);
		return;
	}
	envpool[idx]=255;
	if(totallen%256==255) envpool[++idx]=1;
	envpool[++idx]=0;
	sprintf(envpool+idx,"%s=%s",var,val);
	da_append(env,&envpool[idx]);
}

void expand_env(StrArr env,StrArr* cmd) {
	for(size_t i=0; i<cmd->len; i++) {
		// TODO expand variables when they're substrings of arguments
		if(cmd->items[i][0]=='$') {
			if(strcmp(cmd->items[i],"$?")==0) {
				cmd->items[i]=retbuf;
				continue;
			}
			char* var=envget(env,cmd->items[i]+1);
			if(var) {
				cmd->items[i]=var;
			} else {
				for(size_t j=i+1; j<cmd->len; j++) {
					cmd->items[j-1]=cmd->items[j];
				}
				cmd->len--;
			}
		}
	}
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

int main(int argc,char** argv,char** envp) {
	// char foo[1024];
	// sprintf(foo,"\"woa\\\\h\"z123");
	// size_t foolen=strlen(foo);
	// size_t fooidx=0;
	// parse_string(foo,&foolen,&fooidx);
	// printf("%s %zu %zu (%.*s)\n",foo,foolen,fooidx,(int)foolen,foo);
	// return 0;
	signal(SIGWINCH,getsize);
	pname=argv[0];
	char* homedir=getenv("HOME");
	char* pathenv=getenv("PATH");
	char pathbuf[PATH_MAX];
	if(homedir==NULL) {
		homedir=malloc(PATH_MAX);
		sprintf(homedir,"/home/%s",getpwuid(getuid())->pw_name);
	}
	if(pathenv==NULL) pathenv="/usr/local/sbin:/usr/local/bin:/usr/bin";
	remove_dir(pname,pname);
	if(strlen(pname)==0 || argc<1) {
		pname="(abysh)";
		fprintf(stderr,"%s: warning: weird environment\n",pname);
	}
	StrArr env={0};
	for(;*envp;envp++) {
		da_append(&env,*envp);
	}
	char* shlvlenv=envget(env,"SHLVL");
	if(shlvlenv==NULL) shlvlenv="";
	int shlvl=atoi(shlvlenv);
	if(shlvl<0) shlvl=0;
	if(shlvl>=999) {
		fprintf(stderr,"%s: warning: shell level (%d) is too high, resetting to 1\n",pname,shlvl);
		shlvl=0;
	}
	shlvl++;
	char shlvlbuf[10];
	sprintf(shlvlbuf,"%d",shlvl);
	envedit(&env,"SHLVL",shlvlbuf);
	StrArr history={0};
	char command[MAX_CMD_LEN];
	char cwd[PATH_MAX];
	char promptpath[PATH_MAX];
	char prompt[PATH_MAX*2];
	StrArr cmd={0};
	StrArr tmpvars={0};
	int status=0;
	while(1) {
		getcwd(cwd,PATH_MAX);
		envedit(&env,"PWD",cwd);
		remove_dir(promptpath,cwd);
		if(promptpath[0]=='\0') strcpy(promptpath,cwd);
		snprintf(prompt,sizeof(prompt),"%s %s > ",pname,promptpath);
		sprintf(retbuf,"%d",WEXITSTATUS(status));
		if(WEXITSTATUS(status)>0) {
			sprintf(prompt+strlen(pname)+strlen(promptpath)+2,"[%s] > ",retbuf);
		}
		readline(prompt,command,history);
		memset(&cmd,0,sizeof(cmd));
		memset(&tmpvars,0,sizeof(tmpvars));
		char* trimmed=command;
		trim(&trimmed);
		add_history(trimmed,&history);
		if(!parse_args(&cmd,&tmpvars,trimmed)) continue;
		if(cmd.len) {
			expand_env(env,&cmd);
			if(strcmp(cmd.items[0],"exit")==0) return 0;
			if(strcmp(cmd.items[0],"hax")==0) {
				printf("breaking your code >:)\n");
				fclose(stdin);
			}
			if(strcmp(cmd.items[0],"cd")==0) {
				char* newdir;
				if(cmd.len==1) {
					newdir=homedir;
				} else {
					newdir=cmd.items[1];
				}
				int res=chdir(newdir);
				if(res<0) {
					fprintf(stderr,"%s: cd %s: %s\n",pname,newdir,strerror(errno));
					status=256;
					continue;
				}
				status=0;
				getcwd(cwd,PATH_MAX);
				envedit(&env,"PWD",cwd);
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
			expand_path(cmd,cwd,pathenv,pathbuf);
			envedit(&env,"_",pathbuf);
			size_t env_len=env.len;
			for(size_t i=0; i<tmpvars.len; i++) {
				da_append(&env,tmpvars.items[i]);
			}
			da_append(&env,NULL);
			pid_t pid=fork();
			if(pid==0) {
				int res=0;
				res=execve(pathbuf,cmd.items,env.items);
				if(res<0) {
					fprintf(stderr,"Unknown command: %s\n",cmd.items[0]);
					return 127;
				}
				fprintf(stderr,"%s: internal error\n",pname);
				return 1;
			} else {
				waitpid(pid,&status,0);
				env.len=env_len;
			}
		} else if(tmpvars.len) {
			for(size_t i=0; i<tmpvars.len; i++) {
				size_t tlen=0;
				size_t arglen=strlen(tmpvars.items[i]);
				for(size_t j=0; j<arglen; j++) {
					if(tmpvars.items[i][j]=='=') {
						tmpvars.items[i][j]='\0';
						break;
					}
					tlen++;
				}
				if(tlen==0) continue;
				if(tlen+1<arglen) {
					envedit(&env,tmpvars.items[i],tmpvars.items[i]+tlen+1);
				} else {
					envedit(&env,tmpvars.items[i],NULL);
				}
			}
		}
	}
}
