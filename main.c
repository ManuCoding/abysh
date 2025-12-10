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

#define VERSION "0.4.0"

#define HIST_BUF_CAP (1024*1024)

typedef struct {
	char** items;
	size_t cap;
	size_t len;
} StrArr;

typedef struct Cmd Cmd;

struct Cmd {
	Cmd* prev;
	Cmd* next;
	StrArr current;
	StrArr tmpvars;
	pid_t pid;
};

typedef struct {
	char* s;
	size_t cap;
	size_t len;
} StrBuf;

#define STRBUF_INIT_CAP 1024
void strbuf_resize(StrBuf* buf,size_t newlen) {
	if(newlen+1>=buf->cap) {
		if(buf->cap) buf->cap*=2;
		else buf->cap=STRBUF_INIT_CAP;
		char* newbuf=malloc(buf->cap);
		if(buf->s) {
			memcpy(newbuf,buf->s,buf->len);
			free(buf->s);
		}
		buf->s=newbuf;
	}
	buf->len=newlen;
}

void strbuf_insertat_len(StrBuf* buf,size_t idx,char* str,size_t len) {
	size_t prevlen=buf->len;
	size_t newlen=buf->len+len;
	if(idx>buf->len) idx=buf->len;
	if(buf->s<=str && str<=buf->s+buf->len) {
		asm("int3");
		if(buf->s+idx<str) newlen=buf->len+idx-len;
	}
	strbuf_resize(buf,newlen);
	memmove(buf->s+buf->len-idx,buf->s+prevlen-idx,prevlen-idx);
	memmove(buf->s+idx,str,len);
}

#define strbuf_insertat(buf,idx,str)    strbuf_insertat_len((buf),(idx),(str),strlen((str)))
#define strbuf_append_len(buf,str,len0) strbuf_insertat_len((buf),(buf)->len,(str),(len0))
#define strbuf_append(buf,str)          strbuf_append_len((buf),(str),strlen(str))

void strbuf_trim(StrBuf* buf) {
	size_t offset=0;
	for(; offset<buf->len && isspace(buf->s[offset]); offset++) {}
	if(offset) strbuf_insertat(buf,offset,buf->s+offset);
	for(; buf->len && isspace(buf->s[buf->len-1]); buf->len--) {}
	buf->s[buf->len]='\0';
}

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
	size_t len=strlen(*str);
	while(len && isspace((*str)[len-1])) {
		len--;
	}
	while(len && (*str)[0] && isspace((*str)[0])) {
		(*str)++;
		len--;
	}
	(*str)[len]='\0';
	return len;
}

bool parse_string(StrBuf* command,size_t* idx) {
	if(*idx>=command->len) return false;
	if(command->s[*idx]!='"') return false;
	size_t startidx=*idx;
	for((*idx)++; *idx<command->len; (*idx)++) {
		if(command->s[*idx]=='"') {
			strbuf_insertat_len(command,startidx,command->s+startidx+1,*idx-startidx);
			strbuf_insertat(command,*idx-1,command->s+*idx+1);
			*idx-=2;
			return true;
		}
		if(command->s[*idx]=='\\') {
			if(*idx+1<command->len) {
				if(command->s[*idx+1]!='"' && command->s[*idx+1]!='\\') continue;
				strbuf_insertat(command,*idx,command->s+*idx+1);
				continue;
			}
			return false;
		}
	}
	return false;
}

bool parse_args(Cmd* cmd,StrBuf* command) {
	strbuf_trim(command);
	size_t tlen=0;
	cmd->current.len=0;
	cmd->tmpvars.len=0;
	int varstart=-1;
	for(size_t i=0; i<command->len; i++) {
		if(command->s[i]=='=') {
			if(cmd->current.len>0) {
				tlen++;
				continue;
			}
			if(tlen) {
				varstart=i-tlen;
				continue;
			}
		}
		if(command->s[i]=='\\') {
			if(i+1>=command->len) {
				tlen++;
				continue;
			}
			strbuf_insertat(command,i,command->s+i+1);
			tlen++;
			continue;
		}
		if(command->s[i]=='#' && tlen==0) return true;
		if(command->s[i]=='|') {
			if(tlen==0 && cmd->current.len==0) {
				fprintf(stderr,"%s: unexpected '|'\n",pname);
				return false;
			}
			if(tlen) {
				command->s[i]='\0';
				da_append(&cmd->current,command->s+i-tlen);
				tlen=0;
			}
			Cmd* next=cmd->next;
			if(next==NULL) {
				next=malloc(sizeof(Cmd));
				memset(next,0,sizeof(Cmd));
				cmd->next=next;
				next->prev=cmd;
			}
			cmd=next;
			cmd->current.len=0;
			cmd->tmpvars.len=0;
			continue;
		}
		if(command->s[i]=='"') {
			size_t idx=i;
			if(!parse_string(command,&i)) {
				fprintf(stderr,"%s: unexpected EOF while looking for matching '\"'\n",pname);
				return false;
			}
			tlen+=i-idx+1;
			continue;
		}
		if(command->s[i]=='~') {
			if((tlen>0 && varstart==0) || (varstart>-1 && varstart-tlen==1)) {
				tlen++;
				continue;
			}
			if(i+1>=command->len || isspace(command->s[i+1]) || command->s[i+1]=='|' || command->s[i+1]=='/') {
				char* homedir=getenv("HOME");
				if(homedir==NULL) {
					strbuf_insertat(command,i,command->s+i+1);
				} else {
					size_t homelen=strlen(homedir);
					strbuf_insertat_len(command,i,homedir,homelen);
					i+=homelen;
					// FIXME this may underflow `i`, BUT IT'S FINE since it immediately gets increased
					// actually every branch of this function should set `i` themselves and `i` would
					// only get incremented at the end
					i--;
				}
				continue;
			}
		}
		if(command->s[i]=='$') {
			size_t idx=i+1;
			char* varvalue=NULL;
			if(idx<command->len && command->s[idx]=='?') {
				varvalue=retbuf;
				idx++;
			} else for(; idx<command->len && (isalnum(command->s[idx]) || command->s[idx]=='_') && !isspace(command->s[idx]); idx++) {}
			idx--;
			if(idx-i==0) {
				tlen++;
				continue;
			}
			if(isdigit(command->s[i+1])) {
				tlen++;
				continue;
			}
			// FIXME actually idrc if I use malloc at this point... prob should be smarter
			char* varnamebuf=malloc(command->len);
			sprintf(varnamebuf,"%.*s=",(int)(idx-i),command->s+i+1);
			for(size_t j=cmd->tmpvars.len; varvalue==NULL && j>0; j--) {
				if(strncmp(varnamebuf,cmd->tmpvars.items[j-1],idx-i+1)==0) {
					varvalue=cmd->tmpvars.items[j-1]+idx-i+1;
				}
			}
			varnamebuf[idx-i]='\0';
			if(varvalue==NULL) varvalue=getenv(varnamebuf);
			if(varvalue==NULL) varvalue="";
			size_t varlen=strlen(varvalue);
			strbuf_insertat(command,i+varlen,command->s+i);
			strbuf_insertat_len(command,i,varvalue,varlen);
			tlen+=varlen;
			i+=varlen;
			i--;
			free(varnamebuf);
			continue;
		}
		if(isspace(command->s[i])) {
			if(tlen) {
				command->s[i]='\0';
				if(varstart>-1) {
					fprintf(stderr,"TODO: actually allocate and stuff\n");
					asm("int3");
					da_append(&cmd->tmpvars,command->s+varstart);
					varstart=-1;
					tlen=0;
					continue;
				}
				da_append(&cmd->current,command->s+i-tlen);
				tlen=0;
			}
		} else {
			tlen++;
		}
	}
	if(tlen) {
		if(varstart) da_append(&cmd->tmpvars,command->s+varstart);
		else da_append(&cmd->current,command->s+command->len-tlen);
	}
	if(cmd->next!=NULL) cmd->next->current.len=0;
	return true;
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
	strbuf_resize(command,0);
	tcsetattr(keys_fd,TCSAFLUSH,&raw);
	char ch=0;
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
								memcpy(command->s,history.items[hist_idx],command->len);
								curlen=command->len;
								idx=curlen;
							}
							edited=false;
							printf("\r\x1b[K%s%*s",prompt,(int)curlen,command->s);
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
								memcpy(command->s,history.items[hist_idx],command->len);
								curlen=command->len;
								idx=curlen;
							} else {
								curlen=0;
								idx=0;
								strbuf_resize(command,0);
							}
							edited=false;
							printf("\r\x1b[K%s%*s",prompt,(int)curlen,command->s);
						}
						break;
					case 'C':
move_right:
						if(idx<curlen) {
							printf("%c",command->s[idx]);
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
							printf("\x1b""7%.*s \x1b""8",(int)(curlen-idx),command->s+idx);
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
				strbuf_resize(command,0);
				strbuf_append(command,"exit");
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
					strbuf_insertat_len(command,idx,command->s+idx+1,curlen-1);
					curlen--;
					printf("\x1b[D\x1b""7%.*s \x1b""8",(int)(curlen-idx+1),command->s+idx-1);
					idx--;
				}
				break;
			case 'K'-'@':
				if(idx>=curlen) break;
				strbuf_resize(&killring,0);
				strbuf_append_len(&killring,command->s+idx,curlen-idx);
				for(size_t i=idx; i<curlen; i++) printf(" ");
				for(size_t i=idx; i<curlen; i++) printf("\b");
				curlen=idx;
				strbuf_resize(command,curlen);
				break;
			case 'L'-'@':
				printf("\x1b[H\x1b[2J%s%*s",prompt,(int)curlen,command->s);
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
				strbuf_resize(&killring,0);
				strbuf_append_len(&killring,command->s,idx);
				for(size_t i=0; i<idx; i++) printf("\b");
				printf("%.*s",(int)(curlen-idx),command->s+idx);
				for(size_t i=0; i<idx; i++) printf(" ");
				for(size_t i=0; i<curlen; i++) printf("\b");
				strbuf_insertat(command,0,command->s+idx);
				fflush(stdout);
				curlen-=idx;
				idx=0;
				break;
			case 'Y'-'@':
				size_t klen=killring.len;
				if(klen==0) break;
				printf("%.*s%s",(int)klen,killring.s,command->s+idx);
				for(size_t i=idx; i<curlen; i++) printf("\b");
				strbuf_insertat_len(command,idx,killring.s,klen);
				fflush(stdout);
				curlen+=klen;
				idx+=klen;
				break;
			default:
				if(ch<' ') continue;
				if(idx<curlen) {
					printf(" %.*s\b",(int)(curlen-idx),command->s+idx);
					for(size_t i=curlen; i>idx; i--) printf("\b");
				}
				curlen++;
				printf("%c",ch);
				fflush(stdout);
				strbuf_insertat_len(command,idx,&ch,1);
				edited=true;
				idx++;
		}
		strbuf_resize(command,curlen);
	}
	for(size_t i=(prompt_len+curlen-idx)/term_width+1; i>0; i--) printf("\r\n");
	tcsetattr(keys_fd,TCSAFLUSH,&initial_state);
	strbuf_resize(command,curlen);
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

void run_command(Cmd* cmd,StrArr* history,char(*cwd)[PATH_MAX],int* status,char* homedir) {
	if(cmd->current.len) {
		int lastpipe[2]={-1,-1};
		int nextpipe[2]={-1,-1};
		int allprocspipe[2];
		pipe(allprocspipe);
		pid_t first=0;
		for(Cmd* current=cmd; current && current->current.len; current=current->next) {
			bool last=current->next==NULL || current->next->current.len==0;
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
					fprintf(stderr,"Unknown command: %s\n",cmd->current.items[0]);
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
					for(Cmd* current=cmd; current && current->current.len; current=current->next) {
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
	} else if(cmd->tmpvars.len) {
		for(size_t i=0; i<cmd->tmpvars.len; i++) {
			size_t tlen=0;
			size_t arglen=strlen(cmd->tmpvars.items[i]);
			for(size_t j=0; j<arglen; j++) {
				if(cmd->tmpvars.items[i][j]=='=') {
					cmd->tmpvars.items[i][j]='\0';
					break;
				}
				tlen++;
			}
			if(tlen==0) continue;
			if(tlen+1<arglen) {
				setenv(cmd->tmpvars.items[i],cmd->tmpvars.items[i]+tlen+1,1);
			} else {
				unsetenv(cmd->tmpvars.items[i]);
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
	char cwd[PATH_MAX];
	char promptpath[PATH_MAX];
	char prompt[PATH_MAX*2];
	Cmd cmd={0};
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
		strbuf_trim(&command);
		add_history(command.s,&history);
		if(!parse_args(&cmd,&command)) continue;
		run_command(&cmd,&history,&cwd,&status,homedir);
	}
}
