/**
 * gpio_server_v3.c  –  “/run” で自動シーケンス開始,
 *                      “/stop” で即時非常停止
 *  gcc gpio_server_v3.c -o gpio_server -lwiringPi -lpthread
 */

 #include <stdio.h>
 #include <stdlib.h>
 #include <string.h>
 #include <unistd.h>
 #include <errno.h>
 #include <pthread.h>
 #include <signal.h>
 #include <sys/socket.h>
 #include <netinet/in.h>
 #include <wiringPi.h>
 #include <softPwm.h>
 
 #define PORT          8080
 #define BACKLOG       5
 #define BUF_SIZE      1024
 #define PWM_RANGE     100
 #define PWM_DUTY66    66
 #define STEP_DELAY    3000   /* ms ─ “数秒” を 3 s に統一 */
 
 /* ───────── 共有状態 ───────── */
 static pthread_mutex_t seq_mtx = PTHREAD_MUTEX_INITIALIZER;
 static volatile sig_atomic_t running        = 0;   /* 排他実行フラグ      */
 static volatile sig_atomic_t emergency_stop = 0;   /* 非常停止要求フラグ  */
 
 /* すべての出力を安全状態 (Low / PWM 0) へ */
 static void all_low(void)
 {
     const int pwm[]  = {0,3, 7, 13};
     const int outs[] = {0,  7, 12, 13, 14};
 
     for (size_t i = 0; i < sizeof(pwm)/sizeof(pwm[0]); ++i)
         softPwmWrite(pwm[i], 0);
 
     for (size_t i = 0; i < sizeof(outs)/sizeof(outs[0]); ++i)
         digitalWrite(outs[i], LOW);
 }
 
 /* ───────── ピン 16 割り込み ISR ───────── */
 static void emergency_isr(void)   /* 立ち上がり検出で呼ばれる */
 {
     emergency_stop = 1;           /* フラグを立てるだけに留める */
 }
 
 /* ───────── 自動シーケンス ───────── */
 static void *run_sequence(void *arg)
 {
     if (pthread_mutex_trylock(&seq_mtx) != 0)
         return NULL;
     running = 1;
 
     do {
         /* Step 1: アーム前進 */
         printf("Step 1: Arm moving forward...\n");
         softPwmWrite(0, 100);
         softPwmWrite(7, 33);
 
         while (digitalRead(2) == LOW) {
             if (emergency_stop) break;
             delay(10);
         }
         if (emergency_stop) {
             printf("Emergency stop triggered! Aborting sequence...\n");
             break;
         }
         delay(100);

         printf("Step 1: Limit reached.\n");
         softPwmWrite(7, 100);

 
         /* Step 2: 注水 */
         delay(STEP_DELAY);
         if (emergency_stop) {
             printf("Emergency stop triggered! Aborting sequence...\n");
             break;
         }
         printf("Step 2: Water injection starting...\n");
         softPwmWrite(3, 5);
 
         /* Step 3: 撹拌開始 */
         delay(STEP_DELAY);
         if (emergency_stop) {
             printf("Emergency stop triggered! Aborting sequence...\n");
             break;
         }
         printf("Step 3: Stirring motor starting...\n");
         softPwmWrite(3, 40);
         softPwmWrite(12, 100);
         softPwmWrite(13, PWM_DUTY66);
 
         /* Step 4: 排水開始 */
         delay(STEP_DELAY);
         if (emergency_stop) {
             printf("Emergency stop triggered! Aborting sequence...\n");
             break;
         }
         printf("Step 4: Draining water...\n");
         softPwmWrite(13, 100);

         digitalWrite(14, HIGH);
 
         /* Step 5: 排水完了 */
         delay(STEP_DELAY);
         if (emergency_stop) {
             printf("Emergency stop triggered! Aborting sequence...\n");
             break;
         }
         printf("Step 5: Draining complete.\n");
         digitalWrite(14, LOW);
 
         /* Step 6: アーム復帰 */
         printf("Step 6: Arm returning...\n");
         softPwmWrite(7, 100);
         softPwmWrite(0, 33);
         
         
         while (digitalRead(15) == LOW) {
             if (emergency_stop) break;
             delay(10);
         }
         if (emergency_stop) {
             printf("Emergency stop triggered! Aborting sequence...\n");
             break;
         }
         softPwmWrite(0, 100);


         printf("Step 6: Return limit reached.\n");
 
         printf("Sequence complete.\n");
 
     } while (0);
 
     all_low();
     running = 0;
     emergency_stop = 0;
     pthread_mutex_unlock(&seq_mtx);
     return NULL;
 }
 
 
 /* ───────── HTTP クライアント処理 ───────── */
 static void handle_client(int fd)
 {
     char buf[BUF_SIZE] = {0};
     int n = read(fd, buf, sizeof(buf) - 1);
     if (n <= 0) { close(fd); return; }
 
     char method[8], path[128];
     sscanf(buf, "%7s %127s", method, path);
 
     /* -------------- /run -------------- */
     if (strcmp(method, "GET") == 0 && strcmp(path, "/run") == 0)
     {
         if (running) {     /* 既に動作中 → 409 */
             const char *busy = "HTTP/1.1 409 Conflict\r\n"
                                "Content-Type: text/plain\r\n"
                                "Connection: close\r\n\r\n"
                                "Sequence already running.\n";
             write(fd, busy, strlen(busy));
         } else {
             const char *ok =  "HTTP/1.1 200 OK\r\n"
                               "Content-Type: text/plain\r\n"
                               "Connection: close\r\n\r\n"
                               "Sequence started.\n";
             write(fd, ok, strlen(ok));
 
             pthread_t th;
             pthread_create(&th, NULL, run_sequence, NULL);
             pthread_detach(th);
         }
         close(fd);
         return;
     }
 
     /* -------------- /stop -------------- */
     if (strcmp(method, "GET") == 0 && strcmp(path, "/stop") == 0)
     {
         /* フラグを立てるだけで即応答 */
         emergency_stop = 1;
 
         /* 進行中でなくても安全状態にしておく */
         if (!running) {
             pthread_mutex_lock(&seq_mtx);
             all_low();
             pthread_mutex_unlock(&seq_mtx);
         }
 
         const char *ok = "HTTP/1.1 200 OK\r\n"
                          "Content-Type: text/plain\r\n"
                          "Connection: close\r\n\r\n"
                          "Emergency stop activated.\n";
         write(fd, ok, strlen(ok));
         close(fd);
         return;
     }
 
     /* -------------- その他 -------------- */
     const char *nf = "HTTP/1.1 404 Not Found\r\n"
                      "Content-Type: text/plain\r\n"
                      "Connection: close\r\n\r\n"
                      "Not found.\n";
     write(fd, nf, strlen(nf));
     close(fd);
 }
 
 /* ───────── メイン ───────── */
 int main(void)
 {
     /* GPIO 初期化 */
     if (wiringPiSetup() == -1) { perror("wiringPiSetup"); return 1; }
 
     /* 出力ピン初期化 */
     const int outs[] = {0, 3, 7, 12, 13, 14};
     for (size_t i = 0; i < sizeof(outs) / sizeof(outs[0]); ++i) {
         pinMode(outs[i], OUTPUT);
         digitalWrite(outs[i], LOW);
     }
     softPwmCreate(0, 0, PWM_RANGE);
     softPwmCreate(3, 0, 50);//insert servo

     softPwmCreate(7, 0, PWM_RANGE);
     softPwmCreate(13,0, PWM_RANGE);
 
     /* 入力ピン初期化 */
     pinMode(2,  INPUT);
     pinMode(15, INPUT);
     pinMode(16, INPUT);
 
     /* 非常停止割り込み (ピン 16 立ち上がり) */
     if (wiringPiISR(16, INT_EDGE_RISING, &emergency_isr) < 0) {
         perror("wiringPiISR"); return 1;
     }
 
     /* ソケット準備 */
     int sfd = socket(AF_INET, SOCK_STREAM, 0);
     if (sfd < 0) { perror("socket"); return 1; }
     int opt = 1;
     setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
 
     struct sockaddr_in addr = {
         .sin_family      = AF_INET,
         .sin_addr.s_addr = htonl(INADDR_ANY),
         .sin_port        = htons(PORT)
     };
     if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
         perror("bind"); return 1;
     }
     listen(sfd, BACKLOG);
     printf("Listening on :%d\n", PORT);
 
     /* メインループ */
     while (1) {
         int cfd = accept(sfd, NULL, NULL);
         if (cfd < 0) {
             if (errno == EINTR) continue;
             perror("accept"); break;
         }
         handle_client(cfd);
     }
 
     close(sfd);
     return 0;
 }
 
