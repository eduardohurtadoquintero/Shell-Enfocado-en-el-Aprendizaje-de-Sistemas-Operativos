#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#define MAX_LINEA 256
#define MAX_ARGS 32
#define MAX_CMDS 5     /* maximo de procesos conectados por pipas */
#define MAX_PROC 100   /* maximo de procesos simulados en la ready queue */
#define QUANTUM 10     /* time quantum de Round-Robin (ut) */

/* ====================================================================
   PARTE 1: ANALISIS DE LA LINEA (comillas y tuberias)
   ==================================================================== */

/* Separa una cadena en argumentos respetando comillas simples y dobles.
   Modifica la cadena original. Devuelve el numero de argumentos. */
int separar(char *linea, char **args, int max)
{
    int n = 0;
    char *src = linea;
    char *dst = linea;
    char comilla = '\0';

    while (*src != '\0' && n < max - 1) {
        while (isspace((unsigned char)*src))
            src++;

        if (*src == '\0')
            break;

        args[n++] = dst;

        while (*src != '\0') {
            if (comilla != '\0') {
                if (*src == comilla) {
                    comilla = '\0';
                    src++;
                } else {
                    *dst++ = *src++;
                }
            } else {
                if (*src == '\'' || *src == '"') {
                    comilla = *src++;
                } else if (isspace((unsigned char)*src)) {
                    break;
                } else {
                    *dst++ = *src++;
                }
            }
        }

        if (isspace((unsigned char)*src))
            src++;

        *dst++ = '\0';
    }

    args[n] = NULL;
    return n;
}

/* Divide la linea en comandos usando '|' (ignorando las que estan entre
   comillas). Reemplaza cada '|' por '\0' y guarda el inicio de cada
   comando en cmds[]. Devuelve el numero de comandos, o -1 si hay mas
   de max. */
int dividir_tuberias(char *linea, char **cmds, int max)
{
    int n = 0;
    char comilla = '\0';

    cmds[n++] = linea;

    for (char *p = linea; *p != '\0'; p++) {
        if (comilla != '\0') {
            if (*p == comilla)
                comilla = '\0';
        } else if (*p == '\'' || *p == '"') {
            comilla = *p;
        } else if (*p == '|') {
            if (n >= max)
                return -1;
            *p = '\0';
            cmds[n++] = p + 1;
        }
    }

    return n;
}

/* ====================================================================
   PARTE 2: SIMULACION DE CREACION Y CALENDARIZACION DE PROCESOS
   ==================================================================== */

typedef enum { NEW, READY, RUNNING, TERMINATED } Estado;

static const char *nombre_estado[] = { "new", "ready", "running", "terminated" };

typedef struct {
    int id;        /* identificador numerico */
    int burst;     /* tiempo en CPU (ut) */
    int restante;  /* tiempo que le falta (para Round-Robin) */
    int tamano;    /* bloques de memoria que ocupa */
    int espera;    /* waiting time */
    int retorno;   /* turnaround time */
    Estado estado;
} Proceso;

static Proceso lista[MAX_PROC];
static int total = 0;

/* Convierte una cadena a entero positivo (1..max). Devuelve 1 si es valida. */
static int leer_entero(const char *s, long max, long *out)
{
    char *fin;
    long v = strtol(s, &fin, 10);

    if (*s == '\0' || *fin != '\0' || v <= 0 || v > max)
        return 0;

    *out = v;
    return 1;
}

/* Verifica si ya existe un proceso con ese identificador */
static int existe_id(int id)
{
    for (int i = 0; i < total; i++) {
        if (lista[i].id == id)
            return 1;
    }
    return 0;
}

/* Vacia la ready queue */
static void vaciar_lista(void)
{
    total = 0;
    printf("La ready queue fue vaciada.\n");
}

/* mkprocess <identificador> <burst time> <tamano>
   Crea un proceso en estado new. a = argumentos, na = cuantos hay. */
static void mkprocess(char **a, int na)
{
    if (na != 4) {
        printf("Uso: mkprocess <identificador> <burst time> <tamano>\n");
        return;
    }

    long id, burst, tamano;

    if (!leer_entero(a[1], 999999, &id)) {
        printf("Error: el identificador debe ser un entero entre 1 y 999999.\n");
        return;
    }

    if (!leer_entero(a[2], 100000, &burst)) {
        printf("Error: el burst time debe ser un entero entre 1 y 100000.\n");
        return;
    }

    if (!leer_entero(a[3], 100000, &tamano)) {
        printf("Error: el tamano debe ser un entero entre 1 y 100000 bloques.\n");
        return;
    }

    if (total >= MAX_PROC) {
        printf("La ready queue esta llena (max %d procesos).\n", MAX_PROC);
        return;
    }

    if (existe_id((int)id)) {
        printf("Error: ya existe un proceso con el identificador %ld.\n", id);
        return;
    }

    lista[total].id = (int)id;
    lista[total].burst = (int)burst;
    lista[total].restante = (int)burst;
    lista[total].tamano = (int)tamano;
    lista[total].espera = 0;
    lista[total].retorno = 0;
    lista[total].estado = NEW;
    total++;

    printf("Proceso %ld creado (burst = %ld ut, tamano = %ld bloques, estado: new).\n",
           id, burst, tamano);
}

/* lstprocss: lista todos los procesos creados */
static void lstprocss(void)
{
    if (total == 0) {
        printf("No hay procesos creados.\n");
        return;
    }

    printf("\nProcesos creados:\n");
    printf("+----+----------+-------+---------+------------+\n");
    printf("| #  | ID       | Burst | Tamano  | Estado     |\n");
    printf("+----+----------+-------+---------+------------+\n");

    for (int i = 0; i < total; i++)
        printf("| %-2d | %-8d | %5d | %7d | %-10s |\n",
               i + 1, lista[i].id, lista[i].burst,
               lista[i].tamano, nombre_estado[lista[i].estado]);

    printf("+----+----------+-------+---------+------------+\n");
}

/* Muestra resultados con waiting y turnaround */
static void imprimir_tabla(Proceso p[], int n)
{
    double espera = 0, retorno = 0;

    printf("\n+----------+-------+---------+------------+\n");
    printf("| ID       | Burst | Waiting | Turnaround |\n");
    printf("+----------+-------+---------+------------+\n");

    for (int i = 0; i < n; i++) {
        printf("| %-8d | %5d | %7d | %10d |\n",
               p[i].id, p[i].burst, p[i].espera, p[i].retorno);

        espera += p[i].espera;
        retorno += p[i].retorno;
    }

    printf("+----------+-------+---------+------------+\n");
    printf("| PROMEDIO |       | %7.2f | %10.2f |\n",
           espera / n, retorno / n);
    printf("+----------+-------+---------+------------+\n");
}

/* Verifica que existan procesos */
static int hay_procesos(void)
{
    if (total == 0) {
        printf("No hay procesos en la ready queue.\n");
        return 0;
    }

    return 1;
}

/* Copia los procesos para simular (new -> ready) sin alterar la lista */
static void preparar_copia(Proceso p[])
{
    for (int i = 0; i < total; i++) {
        p[i] = lista[i];
        p[i].restante = p[i].burst;
        p[i].estado = READY;
    }
}

/* FCFS: First-Come, First-Served */
static void fcfs(void)
{
    if (!hay_procesos())
        return;

    Proceso p[MAX_PROC];
    int tiempo = 0;

    preparar_copia(p);

    printf("\n=== FCFS ===\n");

    for (int i = 0; i < total; i++) {
        printf("[t = %3d] Entra al CPU: proceso %d (ready -> running)\n",
               tiempo, p[i].id);

        p[i].espera = tiempo;
        tiempo += p[i].burst;
        p[i].retorno = tiempo;
        p[i].estado = TERMINATED;

        printf("[t = %3d] Sale del CPU: proceso %d (running -> terminated)\n",
               tiempo, p[i].id);
    }

    imprimir_tabla(p, total);
    vaciar_lista();
}

/* SJF: Shortest-Job-First */
static void sjf(void)
{
    if (!hay_procesos())
        return;

    Proceso p[MAX_PROC];
    int tiempo = 0;

    preparar_copia(p);

    /* Ordenar por burst menor a mayor (insercion, estable) */
    for (int i = 1; i < total; i++) {
        Proceso aux = p[i];
        int j = i - 1;

        while (j >= 0 && p[j].burst > aux.burst) {
            p[j + 1] = p[j];
            j--;
        }

        p[j + 1] = aux;
    }

    printf("\n=== SJF ===\n");

    for (int i = 0; i < total; i++) {
        printf("[t = %3d] Entra al CPU: proceso %d (ready -> running)\n",
               tiempo, p[i].id);

        p[i].espera = tiempo;
        tiempo += p[i].burst;
        p[i].retorno = tiempo;
        p[i].estado = TERMINATED;

        printf("[t = %3d] Sale del CPU: proceso %d (running -> terminated)\n",
               tiempo, p[i].id);
    }

    imprimir_tabla(p, total);
    vaciar_lista();
}

/* RR: Round-Robin con time quantum de QUANTUM ut */
static void rr(void)
{
    if (!hay_procesos())
        return;

    Proceso p[MAX_PROC];
    int cola[MAX_PROC];
    int inicio = 0, fin = 0, cantidad = 0;
    int tiempo = 0;

    preparar_copia(p);

    /* Llenar la cola en orden de creacion */
    for (int i = 0; i < total; i++) {
        cola[fin] = i;
        fin = (fin + 1) % MAX_PROC;
        cantidad++;
    }

    printf("\n=== Round Robin (Quantum = %d) ===\n", QUANTUM);

    while (cantidad > 0) {
        int i = cola[inicio];

        inicio = (inicio + 1) % MAX_PROC;
        cantidad--;

        printf("[t = %3d] Entra al CPU: proceso %d (ready -> running)\n",
               tiempo, p[i].id);

        /* Ejecutar un quantum o lo que le reste */
        int uso = (p[i].restante < QUANTUM) ? p[i].restante : QUANTUM;

        tiempo += uso;
        p[i].restante -= uso;

        if (p[i].restante > 0) {
            printf("[t = %3d] Sale del CPU: proceso %d (running -> ready, le faltan %d ut)\n",
                   tiempo, p[i].id, p[i].restante);

            /* Regresa al final de la cola */
            p[i].estado = READY;
            cola[fin] = i;
            fin = (fin + 1) % MAX_PROC;
            cantidad++;
        } else {
            printf("[t = %3d] Sale del CPU: proceso %d (running -> terminated)\n",
                   tiempo, p[i].id);

            p[i].estado = TERMINATED;
            p[i].retorno = tiempo;
            p[i].espera = tiempo - p[i].burst;
        }
    }

    imprimir_tabla(p, total);
    vaciar_lista();
}

/* ====================================================================
   PARTE 3: MANUAL DE AYUDA
   ==================================================================== */

/* Manual de ayuda: lista todas las funciones del shell y su sintaxis.
   Al agregar nuevas funciones, anadir aqui su seccion. */
void mostrar_ayuda(void)
{
    printf(
        "\n"
        "=============== MANUAL DE mi_terminal ===============\n"
        "\n"
        "1. COMANDOS INTERNOS\n"
        "   help                 Muestra este manual.\n"
        "   exit                 Cierra el shell (tambien con Ctrl+D).\n"
        "   mkprocess, lstprocss, fcfs, sjf, rr   (ver seccion 5)\n"
        "   Nota: los comandos internos solo funcionan solos, sin pipas.\n"
        "\n"
        "2. EJECUTAR COMANDOS DE BASH\n"
        "   Sintaxis:   comando [argumentos...]\n"
        "   Se puede ejecutar cualquier programa que este en el PATH\n"
        "   (fork + execvp).\n"
        "   Ejemplos:\n"
        "     ls -l\n"
        "     ps aux\n"
        "     grep hola archivo.txt\n"
        "     cut -d, -f1 datos.csv\n"
        "\n"
        "3. TUBERIAS (PIPAS)\n"
        "   Sintaxis:   comando1 | comando2 | ... | comandoN\n"
        "   Conecta la salida de cada comando con la entrada del siguiente.\n"
        "   Maximo %d comandos por linea (%d pipas).\n"
        "   Ejemplos:\n"
        "     ps aux | grep bash | wc -l\n"
        "     ls -l | grep \"\\.c$\" | sort | head -n 3\n"
        "\n"
        "4. COMILLAS\n"
        "   Sintaxis:   'texto con espacios'   o   \"texto con espacios\"\n"
        "   Agrupan un texto como un solo argumento. Un '|' dentro de\n"
        "   comillas no se toma como pipa.\n"
        "   Ejemplos:\n"
        "     grep \"hola mundo\" archivo.txt\n"
        "     echo \"a|b\"\n"
        "\n"
        "5. SIMULACION DE PROCESOS Y PLANIFICACION\n"
        "   mkprocess <identificador> <burst time> <tamano>\n"
        "       Crea un proceso en estado new.\n"
        "         identificador  entero 1-999999, no puede repetirse\n"
        "         burst time     tiempo en CPU (ut), 1-100000\n"
        "         tamano         bloques de memoria, 1-100000\n"
        "       Ejemplo: mkprocess 1 25 8\n"
        "   lstprocss\n"
        "       Lista los procesos creados con su burst, tamano y estado.\n"
        "   fcfs\n"
        "       First-Come, First-Served: ejecuta en orden de llegada.\n"
        "   sjf\n"
        "       Shortest-Job-First: ejecuta primero el de menor burst.\n"
        "   rr\n"
        "       Round-Robin con time quantum de %d ut.\n"
        "   Los comandos fcfs, sjf y rr muestran cuando entra y sale cada\n"
        "   proceso del CPU, la tabla de waiting/turnaround con sus\n"
        "   promedios, y al terminar vacian la ready queue (max %d procesos).\n"
        "   Estados: new -> ready -> running -> terminated.\n"
        "   Los nombres de estos comandos aceptan mayusculas (FCFS, SJF, RR).\n"
        "\n"
        "6. ERRORES COMUNES\n"
        "   Comando vacio en tuberia      Falta un comando junto a '|'.\n"
        "   Error en execvp (x)           El programa 'x' no existe o no se\n"
        "                                 escribio con espacio (ej. grep.c).\n"
        "   Solo se admiten hasta N ...   Se excedio el maximo de pipas.\n"
        "   Ya existe un proceso ...      Identificador de mkprocess repetido.\n"
        "\n"
        "=====================================================\n"
        "\n",
        MAX_CMDS, MAX_CMDS - 1, QUANTUM, MAX_PROC);
}

/* ====================================================================
   PARTE 4: COMANDOS INTERNOS Y BUCLE PRINCIPAL
   ==================================================================== */

/* Ejecuta un comando interno si a[0] lo es. Devuelve 1 si lo manejo,
   0 si no es interno (para ejecutarlo con fork + execvp). */
static int ejecutar_interno(char **a)
{
    int na = 0;
    while (a[na] != NULL)
        na++;

    const char *cmd = a[0];

    if (strcasecmp(cmd, "mkprocess") == 0) {
        mkprocess(a, na);
        return 1;
    }

    int es_sin_args = strcmp(cmd, "help") == 0 ||
                      strcasecmp(cmd, "lstprocss") == 0 ||
                      strcasecmp(cmd, "lstprocess") == 0 ||
                      strcasecmp(cmd, "fcfs") == 0 ||
                      strcasecmp(cmd, "sjf") == 0 ||
                      strcasecmp(cmd, "rr") == 0;

    if (!es_sin_args)
        return 0;

    if (na > 1) {
        printf("El comando '%s' no recibe argumentos.\n", cmd);
        return 1;
    }

    if (strcmp(cmd, "help") == 0)
        mostrar_ayuda();
    else if (strcasecmp(cmd, "fcfs") == 0)
        fcfs();
    else if (strcasecmp(cmd, "sjf") == 0)
        sjf();
    else if (strcasecmp(cmd, "rr") == 0)
        rr();
    else
        lstprocss();

    return 1;
}

int main(void)
{
    char linea[MAX_LINEA];
    char *cmds[MAX_CMDS];
    char *args[MAX_CMDS][MAX_ARGS];

    while (1) {
        printf("mi_terminal> ");
        fflush(stdout);

        if (fgets(linea, sizeof(linea), stdin) == NULL) {
            printf("\n");
            break;
        }

        linea[strcspn(linea, "\n")] = '\0';

        if (linea[0] == '\0')
            continue;

        int n = dividir_tuberias(linea, cmds, MAX_CMDS);

        if (n == -1) {
            fprintf(stderr, "Solo se admiten hasta %d comandos con tuberias\n",
                    MAX_CMDS);
            continue;
        }

        /* Separar los argumentos de cada comando y validar que no esten vacios */
        int valido = 1;
        for (int i = 0; i < n; i++) {
            if (separar(cmds[i], args[i], MAX_ARGS) == 0) {
                fprintf(stderr, "Comando vacio en tuberia\n");
                valido = 0;
                break;
            }
        }
        if (!valido)
            continue;

        /* Comandos internos (solo cuando no hay pipas) */
        if (n == 1) {
            if (strcmp(args[0][0], "exit") == 0)
                break;

            if (ejecutar_interno(args[0]))
                continue;
        }

        /* Crear las n-1 pipas */
        int fd[MAX_CMDS - 1][2];
        int pipas_ok = 1;
        for (int i = 0; i < n - 1; i++) {
            if (pipe(fd[i]) == -1) {
                perror("Error en pipe");
                for (int j = 0; j < i; j++) {
                    close(fd[j][0]);
                    close(fd[j][1]);
                }
                pipas_ok = 0;
                break;
            }
        }
        if (!pipas_ok)
            continue;

        /* Crear un proceso hijo por comando */
        pid_t pids[MAX_CMDS];
        int lanzados = 0;

        for (int i = 0; i < n; i++) {
            pids[i] = fork();

            if (pids[i] == -1) {
                perror("Error en fork");
                break;
            }

            if (pids[i] == 0) {
                /* Entrada desde la pipa anterior */
                if (i > 0)
                    dup2(fd[i - 1][0], STDIN_FILENO);
                /* Salida hacia la pipa siguiente */
                if (i < n - 1)
                    dup2(fd[i][1], STDOUT_FILENO);

                /* Cerrar todos los descriptores de pipas en el hijo */
                for (int j = 0; j < n - 1; j++) {
                    close(fd[j][0]);
                    close(fd[j][1]);
                }

                execvp(args[i][0], args[i]);
                fprintf(stderr, "Error en execvp (%s): ", args[i][0]);
                perror("");
                exit(1);
            }

            lanzados++;
        }

        /* El padre cierra todas las pipas y espera a los hijos */
        for (int j = 0; j < n - 1; j++) {
            close(fd[j][0]);
            close(fd[j][1]);
        }

        for (int i = 0; i < lanzados; i++)
            waitpid(pids[i], NULL, 0);
    }

    return 0;
}