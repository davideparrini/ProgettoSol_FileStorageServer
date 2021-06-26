#include <myqueueconnections.h>
#include <utils.h>
#include <signal.h>
#include <myhashstoragefile.h>

typedef struct {
    int n_openfile;
    int n_closefile;
    int n_readfile;
    int n_readNfile;
    int n_writefile;
    int n_appendfile;
    int n_removefile;
    int n_lockfile;
    int n_unlockfile;

}stats;

#define _POSIX_C_SOURCE 2001112L
#define PATHCONFIG 50


static pthread_cond_t cond_var_request = PTHREAD_COND_INITIALIZER;
static pthread_cond_t cond_var_pipe_WM = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t mutex_request = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t mutex_connections = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t mutex_file = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t mutex_pipe_signal = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t mutex_pipe_WM = PTHREAD_MUTEX_INITIALIZER;

static char configFile_path[NAME_MAX] = {"configs/"};
static char logFile_path[NAME_MAX] = {"logs/fileLog.txt"};
config configurazione;
static hashtable storage;
static list files_rejected;
static stats stats_op;
static int signal_pipe[2];
static int pipeWorker_manager[2];

#define VERDE 1
#define ROSSO 0
static int flag_semaforo = VERDE;

static int flag_SigHup = 0;
static int flag_closeServer = 0;

void showDirConfig();
void setConfigFile(char* s);
void setUpServer(config* Server);
void print_serverConfig();
void cleanup();
static void *sigHandler(void *arg);
void* worker_thread_function(void* args);
void* manager_thread_function(void* args);
int update_fdmax(fd_set set, int fd_num);
void handle_connection(int *pclient_socket);
void do_task(request* req_server,response *feedback);
int task_openFile(request* r, response* feedback);
int task_read_file(request* r, response* feedback);
int task_read_N_file(request* r, response* feedback);
int task_write_file(request* r, response* feedback);
//int task_append_file(request* r, response* feedback);
//int task_unlock_file(request* r, response* feedback);
int task_close_file(request* r, response* feedback);
int task_remove_file(request* r, response* feedback);
void showDirLogs();
void setLogFile();
void create_FileLog();
int sendlistFiletoReject(list removed_files,int fd_toSend);
void init_Stats();

int main(int argc, char *argv[]){

    if(argc < 2){
        printf("Pochi argomenti!\n./server 'configName'\n");
        exit(EXIT_FAILURE);
    }
    //printf("Scegliere un file .txt di configurazione: \n");
    //showDirConfig();
    char c[PATHCONFIG];
    memset(c,0,PATHCONFIG);
    strcpy(c,argv[1]);
    //scanf("%s",c);
    setConfigFile(c);
    setUpServer(&configurazione);
    print_serverConfig();
    init_hash(&storage,configurazione);
    init_list(&files_rejected);
    init_Stats(&stats_op);

    pthread_t thread_manager;
    pthread_t thread_workers[configurazione.n_thread_workers];
    pthread_t sighandler_thread;

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT); 
    sigaddset(&mask, SIGQUIT);
    sigaddset(&mask, SIGHUP);

    if (pthread_sigmask(SIG_SETMASK, &mask,NULL) != 0) {
	    fprintf(stderr, "FATAL ERROR sigmask \n");
        cleanup();
        exit(EXIT_FAILURE); 
    }

    if ((pipe(signal_pipe))==-1) {
	    perror("signalpipe");
        cleanup();
        exit(EXIT_FAILURE); 
    }
    if ((pipe(pipeWorker_manager))==-1) {
	    perror("pipeWM");
        cleanup();
        exit(EXIT_FAILURE); 
    }
    
 

    if(pthread_create(&sighandler_thread,NULL, sigHandler,&mask) == -1){
        perror("Errore creazione sighandler_thread");
        cleanup();
        exit(EXIT_FAILURE);  
    }
    
    if(pthread_create(&thread_manager,NULL, manager_thread_function,NULL) == -1){
        perror("Errore creazione thread_manager");
        cleanup();
        exit(EXIT_FAILURE);  
    }
    for(size_t i = 0; i < configurazione.n_thread_workers;i++){
        if(pthread_create(&thread_workers[i],NULL, worker_thread_function,NULL) == -1){
            perror("Errore creazione thread_workers");
            cleanup();
            exit(EXIT_FAILURE);  
        }
    }

    if(pthread_join(thread_manager,NULL) != 0){
        perror("Errore join thread_manager");
            cleanup();
            exit(EXIT_FAILURE);  
    }
    for(size_t i = 0; i < configurazione.n_thread_workers;i++){
        if(pthread_join(thread_workers[i],NULL) != 0){
            perror("Errore join thread_workers");
            cleanup();
            exit(EXIT_FAILURE);  
        }
    }

    if(pthread_join(sighandler_thread, NULL) != 0){
        perror("Errore join sighandler_thread");
        cleanup();
        exit(EXIT_FAILURE);
    }
   
    //printf("Contenuto directory Logs:\n");
    //showDirLogs();
    //printf("Dare un nome al filelog.txt :\n");
    //scanf("%s",c);
    //setLogFile(c);
    //print_storageServer(storage);
    create_FileLog();
    free_hash(&storage);
    free_list(&files_rejected);
    cleanup();
    return 0;
}


void showDirConfig(){
    DIR * dir;
    if((dir = opendir("./configs")) == NULL){
        perror("Errore apertura configs DIR");
        exit(EXIT_FAILURE);
    }
    struct dirent *file;
    while((errno=0, file = readdir(dir)) != NULL) {      
        if(!isdot(file->d_name)) printf(" -%s\n",file->d_name);
    }
    printf("\n");
    free(file);
    closedir(dir);
}
void setConfigFile(char * s){
    if(s != NULL) strcat(configFile_path,s);
    else{
        fprintf(stderr,"Errore passaggio fileconfig_path\n");
        exit(EXIT_FAILURE);
    }
    strcat(configFile_path,".txt");
}
void setUpServer(config *Server){
    char buff[NAME_MAX];
    FILE* conf;
    memset(buff,0,NAME_MAX );
    if((conf = fopen(configFile_path, "r"))  == NULL){
        perror("Errore apertura file conf");
        exit(EXIT_FAILURE);
    }
    
    if(fgets(buff,NAME_MAX,conf) != NULL ){
        Server->n_thread_workers = atoi(buff);
    }
    else{
        perror("Errore lettura N thread workers");
        exit(EXIT_FAILURE);
    }
    
    if(fgets(buff,NAME_MAX,conf) != NULL ){
        Server->max_n_file = atoi(buff);
    }
    else{
        perror("Errore lettura max n file");
        exit(EXIT_FAILURE);
    }

    if(fgets(buff,NAME_MAX,conf) != NULL ){
        Server->memory_capacity = atof(buff);
    }
    else{
        perror("Errore lettura memory capacity");
        exit(EXIT_FAILURE);
    }

    if(fgets(buff,NAME_MAX,conf) != NULL ){

        strncpy(Server->socket_name, buff,strlen(buff));
    }
    else{
        perror("Errore lettura socket path");
        exit(EXIT_FAILURE);
    }

    fclose(conf);
}
void print_serverConfig(){
    printf("\nNumero di thread workers: %d\n",configurazione.n_thread_workers);
    printf("Numero massimo di file nel server : %d\n",configurazione.max_n_file);
    printf("Capacità di memoria del server (in MegaBytes) : %.2lf Mb\n",configurazione.memory_capacity);
    printf("Nome del socket : %s\n\n",configurazione.socket_name);

}

void cleanup() {
  unlink(configurazione.socket_name);
}


void* manager_thread_function(void* args){
    cleanup();
    atexit(cleanup);
    int server_fd, client_fd, b, sig;
    SA serv_addr;
    

    if((server_fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1){
        perror("Errore Creazione socket_s");
        exit(EXIT_FAILURE);
    }
    memset(&serv_addr, 0, sizeof(SA));
    strncpy(serv_addr.sun_path, configurazione.socket_name,NAME_MAX);
    serv_addr.sun_family = AF_UNIX; 

    if(bind(server_fd,(struct sockaddr*) &serv_addr, sizeof(serv_addr)) == -1){
        perror("Errore Bind socket_s");
        exit(EXIT_FAILURE);
    }

    if(listen(server_fd,50) == -1){
        perror("Errore Listen Socket_s");
        exit(EXIT_FAILURE);
    }
    
    fd_set set, tmpset;
    FD_ZERO(&set);
    FD_ZERO(&tmpset);

    FD_SET(server_fd, &set);        // aggiungo il listener fd al master set
    FD_SET(signal_pipe[0], &set);  // aggiungo il descrittore di lettura della signal_pipe
    FD_SET(pipeWorker_manager[0], &set);
    int fdmax = (server_fd > signal_pipe[0]) ? server_fd : signal_pipe[0];
    fdmax = (fdmax > pipeWorker_manager[0]) ? fdmax : pipeWorker_manager[0];

    printf("Pronto a ricevere connessioni!\n");
    int termina = 0;
    while(!termina){
        pthread_mutex_lock(&mutex_connections);
        tmpset = set;
        pthread_mutex_unlock(&mutex_connections);
        if(select(fdmax+1,&tmpset,NULL,NULL,NULL) == -1){
            perror("Errore in select");
            exit(EXIT_FAILURE);
        }
        for(int i = 0;i <= fdmax;i++){
            if(FD_ISSET(i,&tmpset)){
                if(i == server_fd){ //ascolto
                    if((client_fd = accept(server_fd,NULL, 0)) == -1){
                        perror("Errore Accept socket_s");
                        exit(EXIT_FAILURE);
                    }
                    printf("Ricevuta connessione! Client_fd: %d\n",client_fd);
                    pthread_mutex_lock(&mutex_connections);
                    FD_SET(client_fd,&set);
                    push_q(client_fd);
                    pthread_mutex_unlock(&mutex_connections);
                    if(client_fd > fdmax) fdmax = client_fd;
                } 
                else{
                    if (i == signal_pipe[0]) {
                        // ricevuto un segnale, esco ed inizio il protocollo di terminazione
                        pthread_mutex_lock(&mutex_pipe_signal);
                        if((b = readn(signal_pipe[0],&sig,sizeof(int)) == -1)){
                            perror("Errore readn pipe");
                            exit(EXIT_FAILURE);
                        }
                        pthread_mutex_unlock(&mutex_pipe_signal); 
                        if(sig == SIGINT || sig == SIGQUIT){
                            pthread_mutex_lock(&mutex_request);
                            pthread_cond_broadcast(&cond_var_request);
                            pthread_mutex_unlock(&mutex_request);
                            termina = 1;
                            flag_closeServer = 1;
                        }
                        if(sig == SIGHUP){
                            pthread_mutex_lock(&mutex_connections);
                            close(server_fd);  
                            FD_CLR(server_fd,&set);
                            flag_SigHup = 1;
                            
                            if(isEmpty_q()){
                                pthread_mutex_lock(&mutex_request);
                                flag_closeServer = 1;
                                pthread_cond_broadcast(&cond_var_request);
                                pthread_mutex_unlock(&mutex_request);
                                termina = 1;
                            }
                            pthread_mutex_unlock(&mutex_connections);
                        }   
                    }
                    else{   
                        if(i == pipeWorker_manager[0]){
                            int fd_pipe;
                            pthread_mutex_lock(&mutex_pipe_WM);
                            if((b = readn(pipeWorker_manager[0],&fd_pipe,sizeof(int)) == -1)){
                                perror("Errore readn pipeWM");
                                exit(EXIT_FAILURE);
                            }
                            FD_SET(fd_pipe,&set);
                            if(fdmax < fd_pipe) fdmax = fd_pipe;
                            flag_semaforo = VERDE;
                            pthread_cond_signal(&cond_var_pipe_WM);
                            pthread_mutex_unlock(&mutex_pipe_WM);
                        }
                        else{ //ascolto client-> readn -> metto in coda
                            request r;
                            memset(&r,0,sizeof(request));
                            pthread_mutex_lock(&mutex_request);
                            if((b = readn(i,&r,sizeof(request))) == -1){
                                perror("Errore readn request");
                                exit(EXIT_FAILURE);
                            }
                            if(b == 0){
                                close(i);
                                pthread_mutex_lock(&mutex_connections);
                                FD_CLR(i,&set);
                                if(i == fdmax) fdmax = update_fdmax(set,fdmax);
                                removeConnection_q(i);
                                if(isEmpty_q() && flag_SigHup){
                                    flag_closeServer = 1;
                                    pthread_cond_broadcast(&cond_var_request);
                                }
                                pthread_mutex_unlock(&mutex_connections);
                            }
                            else{
                                r.socket_fd = i;
                                FD_CLR(i,&set);
                                if(i == fdmax) fdmax = update_fdmax(set,fdmax);
                                push_r(&r);
                                pthread_cond_signal(&cond_var_request);
                            }
                            pthread_mutex_unlock(&mutex_request);
                        }
                    }
                }   
            }
            
        }
    }
    return NULL;
}
int update_fdmax(fd_set set, int fd_max){
	int i, max = 0;

	for(i = 0; i <= fd_max; i++){
		if(FD_ISSET(i, &set)){
			if(i > max)
				max = i;
		}
	}
	return max;
}

static void *sigHandler(void *arg) {
    sigset_t *set = (sigset_t*)arg;

    while(1) {
        int sig,b;
        int r = sigwait(set, &sig);
        if (r != 0) {
            errno = r;
            perror("FATAL ERROR 'sigwait'");
            return NULL;
        }

        switch(sig) {
        case SIGINT:
        case SIGHUP:
        case SIGQUIT:
            pthread_mutex_lock(&mutex_pipe_signal);;
            if((b = writen(signal_pipe[1],&sig,sizeof(int))) == -1){
                perror("Errore writen signalpipe");
                exit(EXIT_FAILURE);
            }
            pthread_mutex_unlock(&mutex_pipe_signal);
            close(signal_pipe[1]); 
            return NULL;
        default:  ; 
        }
    }
    return NULL;	   
}

void* worker_thread_function(void* args){
    while (1){
        pthread_mutex_lock(&mutex_request);

        if(!flag_closeServer && isEmpty_r()){
            pthread_cond_wait(&cond_var_request,&mutex_request);
        }
        if(flag_closeServer){
            pthread_mutex_unlock(&mutex_request);
            pthread_exit(NULL);
        }

        request *r;
        memset(&r,0,sizeof(r));   

        r = pop_r();
        pthread_mutex_unlock(&mutex_request);
        
        if(r != NULL){
            response feedback;
            do_task(r,&feedback);
            if(writen(r->socket_fd,&feedback,sizeof(feedback)) == -1){
                perror("Errore writen feedback");
                exit(EXIT_FAILURE);
            }
            pthread_mutex_lock(&mutex_pipe_WM);
            while(flag_semaforo == ROSSO) pthread_cond_wait(&cond_var_pipe_WM,&mutex_pipe_WM);
            flag_semaforo = ROSSO;
            if(writen(pipeWorker_manager[1],&r->socket_fd,sizeof(int)) == -1){
                perror("Errore writen pipeWM");
                exit(EXIT_FAILURE);
            }
            pthread_mutex_unlock(&mutex_pipe_WM);
        }
        else break;  
    }
    return NULL;
}


void do_task(request* r_from_client,response* feedback){
    //In caso di successo ritorna 1 else 0
    memset(feedback,0,sizeof(response));
    
    switch (r_from_client->type){

    case OPEN_FILE:
        if(task_openFile(r_from_client,feedback)){
            stats_op.n_openfile++;
        }    
        break;
    case READ_FILE:
        if(task_read_file(r_from_client,feedback)){
            stats_op.n_readfile++;
        } 
        break;

    case READ_N_FILE:
        if(task_read_N_file(r_from_client,feedback)){
            stats_op.n_readNfile++;
        }  
        break;
    case WRITE_FILE:
        if(task_write_file(r_from_client,feedback)){
            stats_op.n_writefile++;
        }  
        break;
    case APPEND_FILE:
        if(task_append_file(r_from_client,feedback)){
            stats_op.n_appendfile++;         
        }  
        break;
    
    case REMOVE_FILE:
        if(task_remove_file(r_from_client,feedback)){
            stats_op.n_removefile++;
        }  
        break;
    case CLOSE_FILE:
        if(task_close_file(r_from_client,feedback)){
            stats_op.n_closefile++;
        }  
        break;
    
    case LOCK_FILE:
        //if(task_lock_file(r_from_client,feedback)){
       //     stats_op.n_lockfile++;
       // }  
        break;
    
    case UNLOCK_FILE:
        //if(task_unlock_file(r_from_client,feedback)){
        //    stats_op.n_unlockfile++;
        //}  
        break;
    
    default:
        break;
    }
    //print_storageServer(storage);


}



int task_openFile(request* r, response* feedback){
    //In caso di successo ritorna 1 else 0
    //DA RIGUARDARE O_LOCK
    file_t* f = research_file(storage,r->pathfile);

    switch (r->flags){
 
    case O_CREATE :
        if(f == NULL){
            f = init_file(r->pathfile);
            f->open_flag = 1;
            f->o_create_flag = 1;
            if(!init_file_inServer(&storage,f,&files_rejected)){
                free_file(f);
                feedback->type = NO_SPACE_IN_SERVER;
                return 0;
            }
            else{
                feedback->type =  O_CREATE_SUCCESS;
                if((f->fd = open(f->abs_path, O_RDWR|O_CREAT,0777)) == -1){
                    perror("Errore open in task_open");
                    feedback->type = GENERIC_ERROR;
                    return 0;
                }
                return 1;
            }
        }
        else{
            feedback->type = FILE_ALREADY_EXIST;
            return 0;
        }
        break;
    
    case O_LOCK :
        if(f == NULL){
            feedback->type = O_CREATE_NOT_SPECIFIED_AND_FILE_NEXIST;
            return 0;
        }
        else{
            if(f->locked_flag == 1){
                feedback->type = CANNOT_ACCESS_FILE_LOCKED;
                return 0;
            }
            else{
                if(f->open_flag != 1){
                    f->open_flag = 1;
                    feedback->type = O_LOCK_SUCCESS;
                    if((f->fd = open(f->abs_path, O_RDWR)) == -1){
                        perror("Errore open in task_open");
                        feedback->type = GENERIC_ERROR;
                        return 0;
                    }
                    return 1; 
                }
                else{
                    feedback->type = FILE_ALREADY_OPENED;
                    return 1;
                }
                
            }
        }
        break;

    case O_CREATE|O_LOCK :
        if(f == NULL){
            f = init_file(r->pathfile);
            f->open_flag = 1;
            f->o_create_flag = 1;
            f->locked_flag = 1;
            if(!init_file_inServer(&storage,f,&files_rejected)){
                feedback->type = NO_SPACE_IN_SERVER;
                return 0;
            }
            else{
                feedback->type =  O_CREATE_LOCK_SUCCESS;
                if((f->fd = open(f->abs_path, O_RDWR|O_CREAT,0777)) == -1){
                    perror("Errore open in task_open");
                    feedback->type = GENERIC_ERROR;
                    return 0;
                }
                return 1;
            }
        }
        else{
            feedback->type = FILE_ALREADY_EXIST;
            return 0;
        }
    
        break;

    case O_NOFLAGS :
        if(f == NULL){
            feedback->type = O_CREATE_NOT_SPECIFIED_AND_FILE_NEXIST;
            return 0;        
        }
        else{
            if(f->locked_flag == 1) feedback->type = CANNOT_ACCESS_FILE_LOCKED;
            else{
                if(f->open_flag != 1){
                    f->open_flag = 1;  
                    if((f->fd = open(f->abs_path, O_RDWR)) == -1){
                        perror("Errore open in task_open");
                        feedback->type = GENERIC_ERROR;
                        return 0;
                    }
                    return 1;
                }
                else{
                    feedback->type = FILE_ALREADY_OPENED;
                    return 0;
                }
                
            }
        }   
      
    break;

    default: break;
    }
    printf("Non ci dovrei arrivare qua!\n\n\n\n");
    return  -1;
}

int task_read_file(request* r, response* feedback){
    //In caso di successo ritorna 1 else 0
    int flag_ok = 1;

    file_t* f = research_file(storage,r->pathfile);

    if(f == NULL){
        flag_ok = 0;
        if(writen(r->socket_fd,&flag_ok,sizeof(int)) == -1){
            errno = EAGAIN;
            return 0;
        }
        feedback->type = FILE_NOT_EXIST;
        return 0;
    }
    else{
        
        if(f->content == NULL){
            feedback->type = CANNOT_READ_EMPTY_FILE;
            flag_ok = 0;
        }

        if(f->locked_flag){
            feedback->type = CANNOT_ACCESS_FILE_LOCKED;
            flag_ok = 0;
        } 
        if(!f->open_flag){
            feedback->type = FILE_NOT_OPEN;
            flag_ok = 0;
        }

        if(writen(r->socket_fd,&flag_ok,sizeof(int)) == -1){
            errno = EAGAIN;
            return 0;
        }

        if(!flag_ok) return 0;

        if(writen(r->socket_fd,&f->dim_bytes,sizeof(size_t)) == -1){
            errno = EAGAIN;
            return 0;
        }

        char buffer[f->dim_bytes];
        memset(buffer,0,f->dim_bytes);
        memcpy(buffer,f->content,f->dim_bytes);

        if(writen(r->socket_fd,buffer,f->dim_bytes) == -1){
            perror("SEND CONTENT ERROR IN WRITEN");
            return 0;
        }

        if(f->modified_flag) update_file(&storage,f);
        feedback->type = READ_FILE_SUCCESS;
        feedback->size = f->dim_bytes;
        return 1;
    }
}

int task_read_N_file(request* r, response* feedback){
    //In caso di successo ritorna 1 else 0
    int n_to_read;
    if(r->c <= 0 || r->c >= storage.n_files_free) n_to_read = storage.n_files_free;
    else n_to_read = r->c;

    if(writen(r->socket_fd,&n_to_read,sizeof(int)) == -1){
        errno = EAGAIN;
    }

    size_t h = 0;
    int contatore_file_letti = 0; 

    while(n_to_read > contatore_file_letti){
        list temp;
        if(h < storage.len) temp = storage.cell[h];
        else temp = storage.cache;

        while(temp.head != NULL && n_to_read > 0){
            if(temp.head->open_flag && !temp.head->locked_flag){

                char namefile[NAME_MAX];
                strncpy(namefile,temp.head->abs_path,NAME_MAX);
                if( writen( r->socket_fd, namefile, NAME_MAX) == -1 ){
                    perror("Errore mentre spedisco il nome del file in readNFiles");
                }

                size_t dim_toSend = 0;
                if(temp.head->content != NULL)  dim_toSend = temp.head->dim_bytes;

                if( writen(r->socket_fd, &dim_toSend, sizeof(size_t)) == -1){
                    errno = EAGAIN;
                }
                if(!dim_toSend) dim_toSend = 18;

                char buff[dim_toSend]; 
                memset(buff, 0, dim_toSend);

                if(temp.head->content == NULL)  memcpy(buff,"*NO DATA IN FILE*",18);
                else  memcpy(buff ,temp.head->content, dim_toSend +1);
                
                if( writen( r->socket_fd, buff, dim_toSend + 1) == -1 ){
                    perror("Errore writen send content ");
                }
        
                contatore_file_letti++;
                if(temp.head->modified_flag) update_file(&storage,temp.head);
            } 	
            temp.head = temp.head->next;
		}
        h++;   
    }
    
    if(contatore_file_letti != n_to_read){
        feedback->type = READ_N_FILE_FAILURE;  
        return 0;  
    }
    feedback->type = READ_N_FILE_SUCCESS;
    feedback->c = contatore_file_letti;
    return 1;

}

int task_write_file(request* r, response* feedback){
    //In caso di successo ritorna 1 else 
    file_t* f = research_file(storage,r->pathfile);
    int flag_ok = 0;
    if(f == NULL){
        if(writen(r->socket_fd,&flag_ok,sizeof(int)) == -1){
            errno = EAGAIN;
            return -1;
        }
        feedback->type = FILE_NOT_EXIST;
        return 0;
    }
    else{
        if(!f->locked_flag || !f->open_flag || !f->o_create_flag){
            if(writen(r->socket_fd,&flag_ok,sizeof(int)) == -1){
                errno = EAGAIN;
                return -1;
            }
            feedback->type = WRITE_FILE_FAILURE;
            return 0;
        }
        else{
            
            list removed_files;
            init_list(&removed_files);
            if(ins_file_server(&storage,f,&removed_files)){
                flag_ok = 1;
                if(writen(r->socket_fd,&flag_ok,sizeof(int)) == -1){
                    errno = EAGAIN;
                    return -1;
                }
                if(r->flags){
                    if(!sendlistFiletoReject(removed_files,r->socket_fd)){
                        feedback->type =  CANNOT_SEND_FILES_REJECTED_BY_SERVER;
                        return 0;
                    }
                }
                feedback->type = WRITE_FILE_SUCCESS;
                return 1;  
            }
            else{
                if(writen(r->socket_fd,&flag_ok,sizeof(int)) == -1){
                    errno = EAGAIN;
                    return -1;
                }
                feedback->type = NO_SPACE_IN_SERVER;    
                return 0;
            }
        }
    }
}
int task_append_file(request* r, response* feedback){
    //In caso di successo ritorna 1 else 0
    int flagsOk = 1;
    file_t* file = research_file(storage,r->pathfile);
    if(file == NULL){
        flagsOk = 0;
        if(writen(r->socket_fd,&flagsOk,sizeof(int)) == -1){
            perror("Errore passaggio flagOk in task appendToFile");
            return 0;
        }
        feedback->type = FILE_NOT_EXIST;
    }
    else{
        
        if(file->locked_flag == 1){
            feedback->type = CANNOT_ACCESS_FILE_LOCKED;
            flagsOk = 0;
        }

        if(file->open_flag != 1){
            feedback->type = FILE_NOT_OPEN;
            flagsOk = 0;
        }

        if(writen(r->socket_fd,&flagsOk,sizeof(int)) == -1){
            perror("Errore passaggio flagOk in task appendToFile");
            return 0;
        }

        if(!flagsOk) return 0;

        pthread_mutex_lock(&mutex_file);
        list removed_files;
        init_list(&removed_files);
        if(modifying_file(&storage,file,r->request_size,&removed_files)){
            
            if(fcntl(file->fd,F_SETFL,O_RDWR|O_APPEND) == -1){
                perror("Errore set flag in task_appendFile");
                pthread_mutex_unlock(&mutex_file); 
                return 0;
            }

            char content[r->request_size];
            memset(content,0,r->request_size);

            if(readn(r->socket_fd,&content,r->request_size) == -1){
                perror("GET CONTENT ERROR IN WRITEN 2");
                feedback->type = GENERIC_ERROR;
                fcntl(file->fd,F_SETFL,O_RDWR);
                pthread_mutex_unlock(&mutex_file);
                return 0;
            }
            appendContent(file,content,r->request_size);
            
            if(write(file->fd,content,r->request_size) < 0){
                perror("Errore scrittura file in task_appendToFile");
                feedback->type = GENERIC_ERROR;
                fcntl(file->fd,F_SETFL,O_RDWR);
                pthread_mutex_unlock(&mutex_file); 
                return 0;
            }

            if(fcntl(file->fd,F_SETFL,O_RDWR) == -1){
                perror("Errore set flag in task_appendFile");
                pthread_mutex_unlock(&mutex_file); 
                return 0;
            }
            pthread_mutex_unlock(&mutex_file);

           
            if(r->flags){
                if(!sendlistFiletoReject(removed_files,r->socket_fd)){
                    feedback->type =  CANNOT_SEND_FILES_REJECTED_BY_SERVER; 
                    return 0;
                }
            }
            feedback->type = APPEND_FILE_SUCCESS;  
            return 1;
        }
        else{
            pthread_mutex_unlock(&mutex_file); 
            feedback->type = NO_SPACE_IN_SERVER;  
            return 0;
        }         
    }
    return 0;
}

int task_unlock_file(request* r, response* feedback){
    //In caso di successo ritorna 1 else 0
    return 0;
}
int task_lock_file(request* r, response* feedback){
    //In caso di successo ritorna 1 else 0
    return 0;
}

int task_close_file(request* r, response* feedback){
    //In caso di successo ritorna 1 else 0

    file_t* file = research_file(storage,r->pathfile);
    if(file == NULL){
        feedback->type = FILE_NOT_EXIST;
    }
    else{    
        if(file->open_flag == 0){
            feedback->type = FILE_NOT_OPEN;
            return 0;
        }

        if(close(file->fd) == -1){
            perror("Errore close file, in task_close_file");
            feedback->type = GENERIC_ERROR;
            return 0;
        }

        file->open_flag = 0;
        storage.n_files_free--;
        file->fd = -2;
        if(file->modified_flag) update_file(&storage,file);
        feedback->type = CLOSE_FILE_SUCCESS;
        return 1;
    }

    return 0;
}

int task_remove_file(request* r, response* feedback){
    //In caso di successo ritorna 1 else 0
    int res = 0;
    file_t* file = research_file(storage,r->pathfile);
    if(file == NULL){
        feedback->type = FILE_NOT_EXIST;
    }
    else{
        if(file->locked_flag == 1){
            feedback->type = CANNOT_ACCESS_FILE_LOCKED;
            return res;
        }
        file->locked_flag = 1;
        pthread_mutex_lock(&mutex_file);
        remove_file_server(&storage,file);
        ins_tail_list(&files_rejected,file);
        pthread_mutex_unlock(&mutex_file);
        feedback->type = REMOVE_FILE_SUCCESS;
        res = 1;
    }
    return res;
}

void showDirLogs(){
    DIR * dir;
    if((dir = opendir("./logs")) == NULL){
        perror("Errore apertura configs DIR");
        exit(EXIT_FAILURE);
    }
    struct dirent *file;
    while((errno=0, file = readdir(dir)) != NULL) {      
        if(!isdot(file->d_name)) printf(" -%s\n",file->d_name);
    }
    printf("\n");
    free(file);
    closedir(dir);
}

void setLogFile(char * s){
    if(s != NULL) strcat(logFile_path,s);
    else{
        fprintf(stderr,"Errore passaggio fileconfig_path\n");
        exit(EXIT_FAILURE);
    }
    strcat(logFile_path,".txt");
}

void create_FileLog(){
    
    FILE *f = NULL;
    if((f= fopen(logFile_path,"w+")) == NULL){
        perror("Errore creazione filelog");
    }
    
    fprintf(f,"Numero di file attualmente nel server : %d\n",storage.n_file);
    printf("Numero di file attualmente nel server : %d\n",storage.n_file);
    
    fprintf(f,"Numero di file massimo memorizzato nel server : %d\n",storage.stat_max_n_file);
    printf("Numero di file massimo memorizzato nel server : %d\n",storage.stat_max_n_file);
    
    fprintf(f,"Memoria attualmente utilizzata in Mbytes nel file storage : %.4lf Kb\n",bytesToMb(storage.memory_used));
    printf("Memoria attualmente utilizzata in Mbytes nel file storage : %.4lf Kb\n",bytesToMb(storage.memory_used));

    fprintf(f,"Dimensione massima file in Kbytes raggiunta dal file storage : %.2lf Kb\n",bytesToKb(storage.stat_dim_file));
    printf("Dimensione massima file in Kbytes raggiunta dal file storage : %.2lf Kb\n",bytesToKb(storage.stat_dim_file));

    fprintf(f,"Numero di volte in cui l’algoritmo di rimpiazzamento della cache è stato eseguito per selezionare uno o più file “vittima” : %d\n",storage.stat_n_replacing_algoritm);
    printf("Numero di volte in cui l’algoritmo di rimpiazzamento della cache è stato eseguito per selezionare uno o più file “vittima” : %d\n",storage.stat_n_replacing_algoritm);  
    
    fprintf(f,"\nLista dei file contenuti nello storage al momento della chiusura del server:\n\n");
    printf("\nLista dei file contenuti nello storage al momento della chiusura del server:\n\n");
	
    for (int i=0; i < storage.len; i++){
		if(storage.cell[i].head != NULL){
			list temp = storage.cell[i];
			while(temp.head != NULL){
				fprintf(f,"- %s\n",temp.head->abs_path);
                printf("- %s\n",temp.head->abs_path);
				temp.head = temp.head->next;
			}
		}
	}
    fprintf(f,"\nin cache\n\n");
    printf("\nin cache:\n\n");
    list temp = storage.cache;
    while (temp.head != NULL){
        fprintf(f,"- %s\n",temp.head->abs_path);
        printf("- %s\n",temp.head->abs_path);
		temp.head = temp.head->next;
    }

    fprintf(f,"\n\n");
    printf("\n\n");

    fprintf(f,"Lista file espulsi dal server:\n\n");
    printf("Lista file espulsi dal server:\n\n");

    if(isEmpty(files_rejected)){
        fprintf(f,"**non sono stati espulsi file dal server**\n\n");
        printf("**non sono stati espulsi file dal server**\n\n");
    }    
    while(!isEmpty(files_rejected)){
        fprintf(f,"*rejected* %s\n",files_rejected.head->abs_path);
        printf("*rejected* %s\n",files_rejected.head->abs_path);
        file_t* temp = files_rejected.head;
        files_rejected.head = files_rejected.head->next;
        free_file(temp);
    }
    

    fprintf(f,"\nStatistiche operazioni:\n\n");
    printf("\nStatistiche operazioni:\n\n");

    fprintf(f,"Numero di operazioni di openFile : %d\n",stats_op.n_openfile);
    printf("Numero di operazioni di openFile : %d\n",stats_op.n_openfile);

    fprintf(f,"Numero di operazioni di closeFile : %d\n",stats_op.n_closefile);
    printf("Numero di operazioni di closeFile : %d\n",stats_op.n_closefile);

    fprintf(f,"Numero di operazioni di readFile : %d\n",stats_op.n_readfile);
    printf("Numero di operazioni di readFile : %d\n",stats_op.n_readfile);

    fprintf(f,"Numero di operazioni di readNFile : %d\n",stats_op.n_readNfile);
    printf("Numero di operazioni di readNFile : %d\n",stats_op.n_readNfile);

    fprintf(f,"Numero di operazioni di writeFile : %d\n",stats_op.n_writefile);
    printf("Numero di operazioni di writeFile : %d\n",stats_op.n_writefile);

    fprintf(f,"Numero di operazioni di appendFile : %d\n",stats_op.n_appendfile);
    printf("Numero di operazioni di appendFile : %d\n",stats_op.n_appendfile);

    fprintf(f,"Numero di operazioni di removeFile : %d\n",stats_op.n_removefile);
    printf("Numero di operazioni di removeFile : %d\n",stats_op.n_removefile);

    fprintf(f,"Numero di operazioni di lockFile : %d\n",stats_op.n_lockfile);
    printf("Numero di operazioni di lockFile : %d\n",stats_op.n_lockfile);

    fprintf(f,"Numero di operazioni di unlockFile : %d\n",stats_op.n_unlockfile);
    printf("Numero di operazioni di unlockFile : %d\n\n",stats_op.n_unlockfile);

    fprintf(f,"Server chiuso tramite ricezione di :");
    printf("Server chiuso tramite ricezione di :");
    if(flag_closeServer && flag_SigHup){
        fprintf(f,"   SIGHUP\n\n");
        printf("   SIGHUP\n\n");
    }
    else{
        fprintf(f,"   SIGINT or SIGQUIT\n\n");
        printf("   SIGINT or SIGQUIT\n\n");
    }
    fclose(f);
}


void init_Stats(stats* statistiche_operazioni){

    statistiche_operazioni->n_openfile = 0;
    statistiche_operazioni->n_closefile = 0;
    statistiche_operazioni->n_readfile = 0;
    statistiche_operazioni->n_readNfile = 0;
    statistiche_operazioni->n_writefile = 0;
    statistiche_operazioni->n_appendfile = 0;
    statistiche_operazioni->n_removefile = 0;
    statistiche_operazioni->n_lockfile = 0;
    statistiche_operazioni->n_unlockfile = 0;

}


int sendlistFiletoReject(list removed_files,int fd_toSend){

    int nToSend = removed_files.size;
    if(writen(fd_toSend,&nToSend,sizeof(int)) == -1){
        perror("Errore writen nToSend in writeFile");
        return 0;
    }

    while(nToSend > 0){
		file_t* temp = removed_files.head;
        char pathfile[NAME_MAX];
        memset(pathfile,0,NAME_MAX);
        strncpy(pathfile,temp->abs_path,NAME_MAX);

        if(writen(fd_toSend,pathfile,NAME_MAX) == -1){
            perror("Errore writen abspath in writeFile");
            return 0;
        }

        if(writen(fd_toSend,&temp->dim_bytes,sizeof(size_t)) == -1){
            perror("Errore writen size in writeFile");
            return 0;
        }

        char content[temp->dim_bytes];
        memset(content,0,temp->dim_bytes);
        memcpy(content,temp->content,temp->dim_bytes);
        if(writen(fd_toSend,&content,temp->dim_bytes) == -1){
            perror("Errore writen content in writeFile");
            return 0;
        }

		removed_files.head = removed_files.head->next;
		free_file(temp);
        nToSend--;
    }
    return 1;
}

