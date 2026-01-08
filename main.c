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
#define VERSION "0.4.0"

#define HIST_BUF_CAP (1024*1024)

typedef struct {
	char** items;
	size_t cap;
	size_t len;
} StrArr;

typedef struct {
	char* items;
	size_t cap;
	size_t len;
} StrBuf;

typedef struct {
	StrArr current;
	StrArr tmpvars;
	pid_t pid;
} Cmd;

typedef struct {
	Cmd* items;
	size_t cap;
	size_t len;
	size_t longest;
} Cmds;

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
char retbuf[1024];
static char histbuf[HIST_BUF_CAP];
size_t histidx=0;
static char pathbuf[PATH_MAX];

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

bool parse_string(char* command,size_t len,size_t* idx,StrBuf* parsedcmd) {
	if(*idx>=len) return false;
	if(command[*idx]!='"') return false;
	for((*idx)++; *idx<len; (*idx)++) {
		if(command[*idx]=='"') {
			return true;
		}
		if(command[*idx]=='\\') {
			if(*idx+1<len) {
				if(command[*idx+1]=='"' || command[*idx+1]=='\\') {
					da_append(parsedcmd,command[++*idx]);
					continue;
				}
				da_append(parsedcmd,'\\');
				continue;
			}
			return false;
		}
		da_append(parsedcmd,command[*idx]);
	}
	return false;
}

bool parse_args(Cmds* cmds,char* command,StrBuf* parsedcmd) {
	size_t len=trim(&command);
	size_t tlen=0;
	parsedcmd->len=0;
	char* varstart=NULL;
	size_t argstart=0;
	struct {
		int* items;
		size_t cap;
		size_t len;
	} indexes={0};
	bool parsingenv=true;
	bool result=true;
	for(size_t i=0; i<len; i++) {
		if(command[i]=='=') {
			if(!parsingenv) {
				tlen++;
				goto addchr;
			}
			if(tlen) {
				varstart=command+i-tlen;
				goto addchr;
			}
		}
		if(command[i]=='\\') {
			if(i+1>=len) {
				tlen++;
				goto addchr;
			}
			da_append(parsedcmd,command[++i]);
			tlen++;
			continue;
		}
		if(command[i]=='#' && tlen==0) break;
		if(command[i]=='|') {
			if(tlen==0 && parsingenv) {
				fprintf(stderr,"%s: unexpected '|'\n",pname);
				result=false;
				break;
			}
			if(tlen) {
				da_append(parsedcmd,'\0');
				da_append(&indexes,(int)argstart);
				argstart=parsedcmd->len;
				tlen=0;
			}
			da_append(&indexes,-2);
			parsingenv=true;
			continue;
		}
		if(command[i]=='"') {
			size_t lastidx=i;
			if(!parse_string(command,len,&lastidx,parsedcmd)) {
				fprintf(stderr,"%s: unexpected EOF while looking for matching '\"'\n",pname);
				return false;
			}
			tlen+=lastidx-i-1;
			i=lastidx;
			continue;
		}
		if(command[i]=='~') {
			if((tlen>0 && varstart==NULL) || (varstart && varstart-command-tlen==1)) {
				tlen++;
				goto addchr;
			}
			if(i+1>=len || isspace(command[i+1]) || command[i+1]=='|' || command[i+1]=='/') {
				char* homedir=getenv("HOME");
				if(homedir!=NULL) {
					size_t homelen=strlen(homedir);
					for(size_t i=0; i<homelen; i++) {
						da_append(parsedcmd,homedir[i]);
					}
					tlen+=homelen;
					continue;
				}
				goto addchr;
			}
		}
		if(command[i]=='$') {
			size_t varend=i+1;
			if(varend<len && command[varend]=='?') {
				for(size_t j=0; retbuf[j]!='\0'; j++) {
					da_append(parsedcmd,retbuf[j]);
				}
				i++;
				continue;
			}
			while(varend<len && isalnum(command[varend])) varend++;
			if(varend==i+1) {
				tlen++;
				goto addchr;
			}
			bool incmd=(varstart==NULL);
			bool found=false;
			size_t parsedlen=parsedcmd->len;
			for(size_t j=indexes.len; j>0 && indexes.items[j-1]!=-2; j--) {
				if(indexes.items[j-1]==-1) {
					incmd=false;
					continue;
				}
				if(incmd) continue;
				char* eq=strchr(parsedcmd->items+indexes.items[j-1],'=');
				if(eq==NULL) continue;
				if(eq-parsedcmd->items!=(int)(varend-i-1)) continue;
				if(strncmp(parsedcmd->items+indexes.items[j-1],command+i+1,varend-i-1)!=0) continue;
				for(size_t k=indexes.items[j-1]+varend-i; k<parsedlen && parsedcmd->items[k]!='\0'; k++) {
					da_append(parsedcmd,parsedcmd->items[k]);
					tlen++;
				}
				found=true;
				break;
			}
			if(found) {
				i=varend-1;
				continue;
			}
			for(size_t j=i+1; j<varend; j++) {
				da_append(parsedcmd,command[j]);
			}
			da_append(parsedcmd,'\0');
			char* var=getenv(parsedcmd->items+parsedlen);
			parsedcmd->len=parsedlen;
			if(var==NULL) continue;
			size_t varlen=strlen(var);
			for(size_t j=0; j<varlen; j++) {
				da_append(parsedcmd,var[j]);
				tlen++;
			}
			i=varend-1;
			continue;
		}
		if(isspace(command[i])) {
			if(tlen==0) continue;
			if(varstart) {
				varstart=NULL;
			} else {
				if(parsingenv) da_append(&indexes,-1);
				parsingenv=false;
			}
			da_append(&indexes,argstart);
			da_append(parsedcmd,'\0');
			argstart=parsedcmd->len;
			tlen=0;
			continue;
		} else {
			tlen++;
		}
addchr:
		da_append(parsedcmd,command[i]);
	}
	// FIXME this logic is kinda messed up, BUT IT WORKS
	while(tlen) {
		tlen=0;
		da_append(parsedcmd,'\0');
		if(parsingenv && varstart) da_append(&indexes,(int)(varstart-command));
		if(parsingenv) da_append(&indexes,-1);
		if(varstart) break;
		da_append(&indexes,(int)argstart);
	}
	parsingenv=true;
	if(cmds->longest==0) {
		da_append(cmds,(Cmd) {0});
		cmds->longest=1;
	}
	cmds->len=1;
	Cmd* cmd=&cmds->items[0];
	cmd->current.len=0;
	cmd->tmpvars.len=0;
	for(size_t curcmd=0,i=0; i<indexes.len; i++) {
		if(indexes.items[i]==-1) {
			parsingenv=false;
			continue;
		}
		if(indexes.items[i]==-2) {
			curcmd++;
			while(curcmd>=cmds->longest) {
				da_append(cmds,(Cmd) {0});
				cmds->longest++;
			}
			cmd=&cmds->items[curcmd];
			cmd->current.len=0;
			cmd->tmpvars.len=0;
			parsingenv=true;
			continue;
		}
		if(parsingenv) da_append(&cmd->tmpvars,parsedcmd->items+indexes.items[i]);
		else da_append(&cmd->current,parsedcmd->items+indexes.items[i]);
	}
	return result;
}

void expand_path(StrArr cmd,char* cwd,char* pathenv,char* pathbuf) {
	if(pathenv==NULL) return;
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

void readline(char* prompt,StrBuf* command,StrArr history) {
	static StrBuf killring={0};
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
	command->len=0;
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
								command->len=0;
								size_t cmdlen=strlen(history.items[hist_idx]);
								for(size_t i=0; i<cmdlen; i++) {
									da_append(command,history.items[hist_idx][i]);
								}
								curlen=command->len;
								idx=curlen;
							}
							edited=false;
							printf("\r\x1b[K%s%.*s",prompt,(int)curlen,command->items);
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
								command->len=0;
								size_t cmdlen=strlen(history.items[hist_idx]);
								for(size_t i=0; i<cmdlen; i++) {
									da_append(command,history.items[hist_idx][i]);
								}
								curlen=command->len;
								idx=curlen;
							} else {
								curlen=0;
								idx=0;
								command->len=0;
							}
							edited=false;
							printf("\r\x1b[K%s%.*s",prompt,(int)curlen,command->items);
						}
						break;
					case 'C':
move_right:
						if(idx<curlen) {
							printf("%c",command->items[idx]);
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
								command->items[i]=command->items[i+1];
							}
							curlen--;
							command->len=curlen;
							printf("\x1b""7%.*s \x1b""8",(int)(curlen-idx),command->items+idx);
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
				command->len=0;
				// FIXME very hacky way of quitting :)
				for(size_t i=0; i<4; i++) {
					da_append(command,"exit"[i]);
				}
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
					for(size_t i=idx-1; i+1<curlen; i++) {
						command->items[i]=command->items[i+1];
					}
					curlen--;
					command->len--;
					printf("\x1b[D\x1b""7%.*s \x1b""8",(int)(curlen-idx+1),command->items+idx-1);
					idx--;
				}
				break;
			case 'K'-'@':
				if(idx>=curlen) break;
				killring.len=0;
				for(size_t i=idx; i<curlen; i++) {
					da_append(&killring,command->items[i]);
					printf(" ");
				}
				for(size_t i=idx; i<curlen; i++) printf("\b");
				curlen=idx;
				command->len=idx;
				break;
			case 'L'-'@':
				printf("\x1b[H\x1b[2J%s%.*s",prompt,(int)curlen,command->items);
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
			case 'U'-'@':
				if(idx==0) break;
				killring.len=0;
				for(size_t i=0; i<idx; i++) {
					da_append(&killring,command->items[i]);
					printf("\b");
				}
				printf("%.*s",(int)(curlen-idx),command->items+idx);
				for(size_t i=0; i<idx; i++) {
					command->items[i]=command->items[i+idx];
					printf(" ");
				}
				for(size_t i=0; i<curlen; i++) printf("\b");
				fflush(stdout);
				curlen-=idx;
				command->len=curlen;
				idx=0;
				break;
			case 'Y'-'@':
				size_t klen=killring.len;
				if(klen==0) break;
				printf("%.*s%s",(int)klen,killring.items,command->items+idx);
				for(size_t i=idx; i<curlen; i++) {
					da_append(&killring,command->items[i]);
					printf("\b");
				}
				command->len=idx;
				for(size_t i=0; i<killring.len; i++) {
					da_append(command,killring.items[i]);
				}
				fflush(stdout);
				curlen+=klen;
				killring.len=klen;
				idx+=klen;
				break;
			default:
				if(ch<' ') continue;
				da_append(command,ch);
				if(idx<curlen) {
					printf(" %.*s\b",(int)(curlen-idx),command->items+idx);
					for(size_t i=curlen; i>idx; i--) {
						command->items[i]=command->items[i-1];
						printf("\b");
					}
					command->items[idx]=ch;
				}
				curlen++;
				printf("%c",ch);
				fflush(stdout);
				edited=true;
				idx++;
		}
	}
	for(size_t i=(prompt_len+curlen-idx)/term_width+1; i>0; i--) printf("\r\n");
	tcsetattr(keys_fd,TCSAFLUSH,&initial_state);
	da_append(command,'\0');
}

void add_history(char* command,StrArr* history) {
	if(history->len>0 && strcmp(command,history->items[history->len-1])==0) return;
	size_t len=strlen(command);
	if(len==0) return;
	// TODO actually use the global histbuf buffer
	char* copy=malloc(len+1);
	memcpy(copy,command,len+1);
	da_append(history,copy);
}

void populate_history(StrArr* history,char* homedir) {
	char histfilename[PATH_MAX];
	sprintf(histfilename,"%s/.abysh_history",homedir);
	FILE* histfile=fopen(histfilename,"r");
	if(histfile==NULL) return;
	ssize_t count=0;
	char* curline=NULL;
	for(size_t n=0,remaining=HIST_BUF_CAP; remaining>0 && (count=getline(&curline,&n,histfile))!=-1 && (size_t)count<remaining; remaining-=count) {
		char* nl=strchr(curline,'\n');
		if(nl) *nl='\0';
		char* line=curline;
		size_t len=trim(&line);
		if(len==0) continue;
		if(history->len>0 && strcmp(history->items[history->len-1],line)==0) continue;
		memcpy(histbuf+histidx,line,len+1);
		da_append(history,histbuf+histidx);
		histidx+=len+1;
	}
	if(curline) free(curline);
	fclose(histfile);
}

bool write_history(StrArr history,char* homedir) {
	char histfilename[PATH_MAX];
	sprintf(histfilename,"%s/.abysh_history",homedir);
	FILE* histfile=fopen(histfilename,"w");
	if(histfile==NULL) return false;
	// removing pesky trailing exits
	while(history.len>0 && strcmp(history.items[history.len-1],"exit")==0) history.len--;
	for(size_t i=0; i<history.len; i++) {
		fprintf(histfile,"%s\n",history.items[i]);
	}
	fclose(histfile);
	return true;
}

void populate_env(StrArr tmpvars) {
	for(size_t i=0; i<tmpvars.len; i++) {
		char* var=tmpvars.items[i];
		char* eq=strchr(var,'=');
		if(var+strlen(var)==eq+1) {
			*eq='\0';
			unsetenv(var);
			continue;
		}
		putenv(var);
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
	fprintf(fd,"    cd directory   Change CWD to directory\n");
	fprintf(fd,"    version        Prints the version of the shell in a single line\n");
	fprintf(fd,"    help           Print this help\n");
}

bool handle_builtin(Cmd cmd,int* status,StrArr history,char* homedir) {
	if(strcmp(cmd.current.items[0],"exit")==0) {
		write_history(history,homedir);
		if(cmd.current.len==1) {
			exit(WEXITSTATUS(*status));
		}
		int res=atoi(cmd.current.items[1]);
		exit(res);
	}
	if(strcmp(cmd.current.items[0],"cd")==0) {
		char* newdir;
		if(cmd.current.len==1) {
			newdir=getenv("HOME");
			if(newdir==NULL || *newdir=='\0') return true;
		} else {
			newdir=cmd.current.items[1];
		}
		int res=chdir(newdir);
		if(res<0) {
			fprintf(stderr,"%s: cd %s: %s\n",pname,newdir,strerror(errno));
			*status=256;
			return true;
		}
		*status=0;
		return true;
	}
	if(strcmp(cmd.current.items[0],"version")==0) {
		version(pname,stdout);
		return true;
	}
	if(strcmp(cmd.current.items[0],"help")==0) {
		help(pname,stdout);
		return true;
	}
	return false;
}

void run_command(Cmds* cmds,StrArr* history,char(*cwd)[PATH_MAX],int* status,char* homedir) {
	if(cmds->len && cmds->items[0].current.len) {
		int lastpipe[2]={-1,-1};
		int nextpipe[2]={-1,-1};
		int allprocspipe[2];
		pipe(allprocspipe);
		pid_t first=0;
		for(size_t i=0; i<cmds->len; i++) {
			bool last=i+1>=cmds->len;
			Cmd* current=&cmds->items[i];
			if(current->current.len==0 || current->current.items[0]==NULL || current->current.items[0][0]=='\0') continue;
			expand_path(current->current,*cwd,getenv("PATH"),pathbuf);
			if(handle_builtin(*current,status,*history,homedir)) continue;
			if(!last) pipe(nextpipe);
			pid_t pid=fork();
			if(pid!=0) {
				if(first==0) first=pid;
				current->pid=pid;
			}
			if(pid==0) {
				int res=0;
				setpgid(0,first);
				close(allprocspipe[0]);
				char* funnychar="E";
				if(last) write(allprocspipe[1],funnychar,1);
				close(allprocspipe[1]);
				if(lastpipe[0]>=0) {
					dup2(lastpipe[0],STDIN_FILENO);
					close(lastpipe[0]);
				}
				if(lastpipe[1]>=0) close(lastpipe[1]);
				if(nextpipe[1]>=0)  {
					dup2(nextpipe[1],STDOUT_FILENO);
					close(nextpipe[1]);
				}
				if(nextpipe[0]>=0) close(nextpipe[0]);
				da_append(&current->current,NULL);
				populate_env(current->tmpvars);
				setenv("_",pathbuf,1);
				res=execv(pathbuf,current->current.items);
				if(res<0) {
					fprintf(stderr,"Unknown command: %s\n",current->current.items[0]);
					exit(127);
				}
				fprintf(stderr,"%s: internal error\n",pname);
				exit(1);
			} else if(last) {
				if(lastpipe[0]>=0) close(lastpipe[0]);
				if(lastpipe[1]>=0) close(lastpipe[1]);
				close(allprocspipe[1]);
				char dummybuf[1];
				read(allprocspipe[0],dummybuf,1);
				close(allprocspipe[0]);
				tcsetpgrp(STDIN_FILENO,first);
				while((pid=waitpid(-first,status,0))>0) {
					char* command="<none>";
					for(size_t i=0; i<cmds->len; i++) {
						Cmd* current=&cmds->items[i];
						if(current->pid==pid) {
							command=current->current.items[0];
							break;
						}
					}
					if(WIFSIGNALED(*status)) {
						int signal=WTERMSIG(*status);
						if(signal==SIGPIPE) continue;
						fprintf(stderr,"child %s (%d) terminated with signal %d (%s)\n",command,pid,signal,strsignal(signal));
					}
				}
				tcsetpgrp(STDIN_FILENO,getpgid(getpid()));
			} else {
				if(lastpipe[0]>=0) close(lastpipe[0]);
				if(lastpipe[1]>=0) close(lastpipe[1]);
				lastpipe[0]=nextpipe[0];
				lastpipe[1]=nextpipe[1];
			}
		}
	} else if(cmds->len && cmds->items[0].tmpvars.len) {
		for(size_t i=0; i<cmds->items[0].tmpvars.len; i++) {
			size_t tlen=0;
			size_t arglen=strlen(cmds->items[0].tmpvars.items[i]);
			for(size_t j=0; j<arglen; j++) {
				if(cmds->items[0].tmpvars.items[i][j]=='=') {
					cmds->items[0].tmpvars.items[i][j]='\0';
					break;
				}
				tlen++;
			}
			if(tlen==0) continue;
			if(tlen+1<arglen) {
				setenv(cmds->items[0].tmpvars.items[i],cmds->items[0].tmpvars.items[i]+tlen+1,1);
			} else {
				unsetenv(cmds->items[0].tmpvars.items[i]);
			}
		}
	}
}

int main(int argc,char** argv) {
	signal(SIGWINCH,getsize);
	signal(SIGTTOU,SIG_IGN);
	pname=argv[0];
	char* homedir=getenv("HOME");
	char* pathenv=getenv("PATH");
	if(homedir==NULL) {
		homedir=malloc(PATH_MAX);
		sprintf(homedir,"/home/%s",getpwuid(getuid())->pw_name);
	}
	if(pathenv==NULL) {
		pathenv="/usr/local/sbin:/usr/local/bin:/usr/bin";
		setenv("PATH",pathenv,1);
	}
	remove_dir(pname,pname);
	if(strlen(pname)==0 || argc<1) {
		pname="(abysh)";
		fprintf(stderr,"%s: warning: weird environment\n",pname);
	}
	char* shlvlenv=getenv("SHLVL");
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
	setenv("SHLVL",shlvlbuf,1);
	StrArr history={0};
	StrBuf command={0};
	StrBuf parsedcmd={0};
	char cwd[PATH_MAX];
	char promptpath[PATH_MAX];
	char prompt[PATH_MAX*2];
	Cmds cmds={0};
	int status=0;
	populate_history(&history,homedir);
	while(1) {
		getcwd(cwd,PATH_MAX);
		setenv("PWD",cwd,1);
		if(strcmp(homedir,cwd)==0) sprintf(promptpath,"~");
		else remove_dir(promptpath,cwd);
		if(promptpath[0]=='\0') strcpy(promptpath,cwd);
		snprintf(prompt,sizeof(prompt),"%s %s > ",pname,promptpath);
		sprintf(retbuf,"%d",WEXITSTATUS(status));
		if(WEXITSTATUS(status)>0) {
			sprintf(prompt+strlen(pname)+strlen(promptpath)+2,"[%s] > ",retbuf);
		}
		readline(prompt,&command,history);
		char* trimmed=command.items;
		trim(&trimmed);
		add_history(trimmed,&history);
		if(!parse_args(&cmds,trimmed,&parsedcmd)) continue;
		run_command(&cmds,&history,&cwd,&status,homedir);
	}
}
