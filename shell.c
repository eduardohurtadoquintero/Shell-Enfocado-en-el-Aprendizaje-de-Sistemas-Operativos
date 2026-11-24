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
#define MEM_BLOQUES 256 /* bloques de memoria totales de la simulacion */
#define MAX_ID 17      /* identificador: 16 caracteres + '\0' */

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

// simulacion y calendarizacion de procesos

typedef enum { NEW, READY, RUNNING, TERMINATED } Estado;

static const char *nombre_estado[] = { "new", "ready", "running", "terminated" };

typedef enum { FIRST_FIT, BEST_FIT, WORST_FIT } Estrategia;

static const char *nombre_estrategia[] = { "first fit", "best fit", "worst fit" };

typedef struct {
    char id[MAX_ID]; /* identificador (texto: A, B, proceso1, ...) */
    int uid;       /* numero interno unico, para marcar bloques de memoria */
    int burst;     /* tiempo en CPU (ut) */
    int restante;  /* tiempo que le falta (para Round-Robin) */
    int tamano;    /* bloques de memoria que ocupa */
    int espera;    /* waiting time */
    int retorno;   /* turnaround time */
    int base;      /* primer bloque en memoria (-1 si no esta en memoria) */
    int orden;     /* orden de llegada a la ready queue (alloc) */
    Estado estado;
} Proceso;

static Proceso lista[MAX_PROC];
static int total = 0;

/* Memoria contigua: memoria[i] guarda el uid del proceso dueno del bloque i
   (0 = bloque libre). */
static int memoria[MEM_BLOQUES];
static int contador_llegada = 0;
static int contador_uid = 0;

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

/* Devuelve la posicion del proceso con ese identificador, o -1 */
static int buscar_id(const char *id)
{
    for (int i = 0; i < total; i++) {
        if (strcmp(lista[i].id, id) == 0)
            return i;
    }
    return -1;
}

/* Un identificador es valido si tiene de 1 a MAX_ID-1 caracteres entre
   letras, numeros, '_' y '-' (sin espacios). */
static int id_valido(const char *s)
{
    size_t n = strlen(s);

    if (n == 0 || n >= MAX_ID)
        return 0;

    for (; *s != '\0'; s++) {
        if (!isalnum((unsigned char)*s) && *s != '_' && *s != '-')
            return 0;
    }

    return 1;
}

/* Interpreta el nombre de la estrategia: first | best | worst
   (tambien firstfit, first-fit, ff, bf, wf; sin importar mayusculas) */
static int leer_estrategia(const char *s, Estrategia *out)
{
    char t[16];
    int n = 0;

    for (; *s != '\0'; s++) {
        if (*s == '-' || *s == '_')
            continue;
        if (n >= 15)
            return 0;
        t[n++] = (char)tolower((unsigned char)*s);
    }
    t[n] = '\0';

    if (strcmp(t, "first") == 0 || strcmp(t, "firstfit") == 0 || strcmp(t, "ff") == 0)
        *out = FIRST_FIT;
    else if (strcmp(t, "best") == 0 || strcmp(t, "bestfit") == 0 || strcmp(t, "bf") == 0)
        *out = BEST_FIT;
    else if (strcmp(t, "worst") == 0 || strcmp(t, "worstfit") == 0 || strcmp(t, "wf") == 0)
        *out = WORST_FIT;
    else
        return 0;

    return 1;
}

/* Bloques libres en total */
static int bloques_libres(void)
{
    int libres = 0;

    for (int i = 0; i < MEM_BLOQUES; i++) {
        if (memoria[i] == 0)
            libres++;
    }

    return libres;
}

/* Tamano del hueco contiguo mas grande */
static int mayor_hueco(void)
{
    int mayor = 0, actual = 0;

    for (int i = 0; i < MEM_BLOQUES; i++) {
        actual = (memoria[i] == 0) ? actual + 1 : 0;
        if (actual > mayor)
            mayor = actual;
    }

    return mayor;
}

/* Busca un hueco de al menos tam bloques segun la estrategia.
   Devuelve el bloque donde empieza, o -1 si ningun hueco alcanza. */
static int buscar_hueco(int tam, Estrategia e)
{
    int mejor = -1, mejor_tam = 0;
    int i = 0;

    while (i < MEM_BLOQUES) {
        if (memoria[i] != 0) {
            i++;
            continue;
        }

        int ini = i;
        while (i < MEM_BLOQUES && memoria[i] == 0)
            i++;

        int hueco = i - ini;

        if (hueco < tam)
            continue;

        if (e == FIRST_FIT)
            return ini;

        if (mejor == -1 ||
            (e == BEST_FIT && hueco < mejor_tam) ||
            (e == WORST_FIT && hueco > mejor_tam)) {
            mejor = ini;
            mejor_tam = hueco;
        }
    }

    return mejor;
}

/* Quita al proceso de memoria (libera sus bloques) */
static void liberar_memoria(Proceso *p)
{
    if (p->base < 0)
        return;

    for (int i = p->base; i < p->base + p->tamano; i++)
        memoria[i] = 0;

    p->base = -1;
}

/* Elimina el proceso de la lista; si estaba en ready libera su memoria */
static void eliminar_indice(int idx)
{
    if (lista[idx].estado == READY)
        liberar_memoria(&lista[idx]);

    for (int i = idx; i < total - 1; i++)
        lista[i] = lista[i + 1];

    total--;
}

static void mkprocess(char **a, int na)
{
    if (na != 4) {
        printf("Uso: mkprocess <identificador> <burst time> <tamano>\n");
        return;
    }

    const char *id = a[1];
    long burst, tamano;

    if (!id_valido(id)) {
        printf("Error: identificador invalido (use letras, numeros, '_' o '-', "
               "maximo %d caracteres).\n", MAX_ID - 1);
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
        printf("La lista de procesos esta llena (max %d procesos).\n", MAX_PROC);
        return;
    }

    if (buscar_id(id) >= 0) {
        printf("Error: ya existe un proceso con el identificador %s.\n", id);
        return;
    }

    snprintf(lista[total].id, MAX_ID, "%s", id);
    lista[total].uid = ++contador_uid;
    lista[total].burst = (int)burst;
    lista[total].restante = (int)burst;
    lista[total].tamano = (int)tamano;
    lista[total].espera = 0;
    lista[total].retorno = 0;
    lista[total].base = -1;
    lista[total].orden = 0;
    lista[total].estado = NEW;
    total++;

    printf("Proceso %s creado (burst = %ld ut, tamano = %ld bloques, estado: new).\n",
           id, burst, tamano);
}

static void alloc(char **a, int na)
{
    if (na != 3) {
        printf("Uso: alloc <identificador> <estrategia>\n");
        printf("     estrategia: first | best | worst\n");
        return;
    }

    const char *id = a[1];
    Estrategia e;

    if (!id_valido(id)) {
        printf("Error: identificador invalido (use letras, numeros, '_' o '-', "
               "maximo %d caracteres).\n", MAX_ID - 1);
        return;
    }

    if (!leer_estrategia(a[2], &e)) {
        printf("Error: estrategia invalida '%s' (use first, best o worst).\n", a[2]);
        return;
    }

    int idx = buscar_id(id);

    if (idx < 0) {
        printf("Error: no existe un proceso con el identificador %s.\n", id);
        return;
    }

    if (lista[idx].estado == READY) {
        printf("El proceso %s ya esta en memoria (base %d).\n", id, lista[idx].base);
        return;
    }

    if (lista[idx].estado == TERMINATED) {
        printf("El proceso %s ya termino y no puede volver a correr.\n", id);
        return;
    }

    int tam = lista[idx].tamano;
    int base = buscar_hueco(tam, e);

    if (base < 0) {
        int libres = bloques_libres();

        if (libres >= tam)
            printf("Error: no hay un hueco contiguo de %d bloques (hay %d libres pero "
                   "fragmentados, el mayor hueco es de %d). Use compact.\n",
                   tam, libres, mayor_hueco());
        else
            printf("Error: memoria insuficiente (necesita %d bloques, quedan %d de %d).\n",
                   tam, libres, MEM_BLOQUES);
        return;
    }

    for (int i = base; i < base + tam; i++)
        memoria[i] = lista[idx].uid;

    lista[idx].base = base;
    lista[idx].estado = READY;
    lista[idx].orden = ++contador_llegada;

    printf("Proceso %s cargado a memoria con %s: bloques %d-%d (estado: ready).\n",
           id, nombre_estrategia[e], base, base + tam - 1);
}

static void cmd_free(char **a, int na)
{
    if (na != 2) {
        printf("Uso: free <identificador>\n");
        return;
    }

    const char *id = a[1];

    if (!id_valido(id)) {
        printf("Error: identificador invalido (use letras, numeros, '_' o '-', "
               "maximo %d caracteres).\n", MAX_ID - 1);
        return;
    }

    int idx = buscar_id(id);

    if (idx < 0) {
        printf("Error: no existe un proceso con el identificador %s.\n", id);
        return;
    }

    if (lista[idx].estado == NEW) {
        printf("El proceso %s no esta en memoria.\n", id);
        return;
    }

    if (lista[idx].estado == TERMINATED) {
        printf("El proceso %s ya termino y ya no esta en memoria.\n", id);
        return;
    }

    int base = lista[idx].base;
    int fin = base + lista[idx].tamano - 1;

    liberar_memoria(&lista[idx]);
    lista[idx].estado = NEW;

    printf("Proceso %s liberado de memoria (bloques %d-%d, estado: new).\n",
           id, base, fin);
}

static void my_kill(char **a, int na)
{
    if (na != 2) {
        printf("Uso: my_kill <identificador>\n");
        return;
    }

    const char *id = a[1];

    if (!id_valido(id)) {
        printf("Error: identificador invalido (use letras, numeros, '_' o '-', "
               "maximo %d caracteres).\n", MAX_ID - 1);
        return;
    }

    int idx = buscar_id(id);

    if (idx < 0) {
        printf("Error: no existe un proceso con el identificador %s.\n", id);
        return;
    }

    if (lista[idx].estado == READY) {
        int base = lista[idx].base;
        int fin = base + lista[idx].tamano - 1;

        eliminar_indice(idx);
        printf("Proceso %s eliminado (se libero su memoria: bloques %d-%d).\n",
               id, base, fin);
    } else {
        eliminar_indice(idx);
        printf("Proceso %s eliminado.\n", id);
    }
}

/* lstprocss: lista todos los procesos creados con su estado y bloques */
static void lstprocss(void)
{
    if (total == 0) {
        printf("No hay procesos creados.\n");
        return;
    }

    printf("\nProcesos creados:\n");
    printf("+----+------------------+-------+---------+-------+------------+\n");
    printf("| #  | ID               | Burst | Bloques | Base  | Estado     |\n");
    printf("+----+------------------+-------+---------+-------+------------+\n");

    for (int i = 0; i < total; i++) {
        char base[12];

        if (lista[i].base >= 0)
            snprintf(base, sizeof(base), "%d", lista[i].base);
        else
            snprintf(base, sizeof(base), "-");

        printf("| %-2d | %-16s | %5d | %7d | %5s | %-10s |\n",
               i + 1, lista[i].id, lista[i].burst,
               lista[i].tamano, base, nombre_estado[lista[i].estado]);
    }

    printf("+----+------------------+-------+---------+-------+------------+\n");
    printf("Memoria: %d/%d bloques ocupados.\n",
           MEM_BLOQUES - bloques_libres(), MEM_BLOQUES);
}

/* mstatus: muestra el estado de la memoria (procesos y huecos) */
static void mstatus(void)
{
    printf("\nEstado de la memoria (%d bloques, asignacion contigua):\n", MEM_BLOQUES);
    printf("+--------+--------+---------+--------------------------+\n");
    printf("| Inicio | Fin    | Tamano  | Contenido                |\n");
    printf("+--------+--------+---------+--------------------------+\n");

    int i = 0, huecos = 0, libres = 0, mayor = 0;

    while (i < MEM_BLOQUES) {
        int ini = i;
        int dueno = memoria[i];

        while (i < MEM_BLOQUES && memoria[i] == dueno)
            i++;

        int tam = i - ini;
        char contenido[32];

        if (dueno == 0) {
            snprintf(contenido, sizeof(contenido), "LIBRE");
            huecos++;
            libres += tam;
            if (tam > mayor)
                mayor = tam;
        } else {
            const char *nombre = "?";

            for (int k = 0; k < total; k++) {
                if (lista[k].uid == dueno)
                    nombre = lista[k].id;
            }

            snprintf(contenido, sizeof(contenido), "Proceso %s", nombre);
        }

        printf("| %-6d | %-6d | %7d | %-24s |\n", ini, i - 1, tam, contenido);
    }

    printf("+--------+--------+---------+--------------------------+\n");
    printf("Ocupados: %d | Libres: %d | Huecos: %d | Hueco mayor: %d\n",
           MEM_BLOQUES - libres, libres, huecos, mayor);

    if (huecos > 1)
        printf("Fragmentacion externa: %.1f%% (la memoria libre esta dividida en %d huecos; "
               "use compact).\n",
               100.0 * (libres - mayor) / libres, huecos);
    else
        printf("Sin fragmentacion externa.\n");
}

/* compact: junta los procesos al inicio de la memoria para unir todos los
   huecos en uno solo */
static void compact(void)
{
    int idx[MAX_PROC];
    int n = 0;

    for (int i = 0; i < total; i++) {
        if (lista[i].estado == READY)
            idx[n++] = i;
    }

    /* Ordenar por posicion actual en memoria (insercion) */
    for (int i = 1; i < n; i++) {
        int aux = idx[i];
        int j = i - 1;

        while (j >= 0 && lista[idx[j]].base > lista[aux].base) {
            idx[j + 1] = idx[j];
            j--;
        }

        idx[j + 1] = aux;
    }

    /* Ver si hace falta mover algo */
    int destino = 0, necesita = 0;

    for (int k = 0; k < n; k++) {
        if (lista[idx[k]].base != destino)
            necesita = 1;
        destino += lista[idx[k]].tamano;
    }

    if (!necesita) {
        printf("La memoria ya esta compactada (no hay fragmentacion).\n");
        return;
    }

    printf("Compactando memoria:\n");

    for (int i = 0; i < MEM_BLOQUES; i++)
        memoria[i] = 0;

    destino = 0;

    for (int k = 0; k < n; k++) {
        Proceso *p = &lista[idx[k]];

        if (p->base != destino)
            printf("  Proceso %s: bloque %d -> %d\n", p->id, p->base, destino);

        p->base = destino;

        for (int i = destino; i < destino + p->tamano; i++)
            memoria[i] = p->uid;

        destino += p->tamano;
    }

    printf("Memoria compactada: un solo hueco libre de %d bloques (bloques %d-%d).\n",
           MEM_BLOQUES - destino, destino, MEM_BLOQUES - 1);
}

/* Muestra resultados con waiting y turnaround por proceso y promedio */
static void imprimir_tabla(Proceso p[], int n)
{
    double espera = 0, retorno = 0;

    printf("\n+------------------+-------+---------+------------+\n");
    printf("| ID               | Burst | Waiting | Turnaround |\n");
    printf("+------------------+-------+---------+------------+\n");

    for (int i = 0; i < n; i++) {
        printf("| %-16s | %5d | %7d | %10d |\n",
               p[i].id, p[i].burst, p[i].espera, p[i].retorno);

        espera += p[i].espera;
        retorno += p[i].retorno;
    }

    printf("+------------------+-------+---------+------------+\n");
    printf("| PROMEDIO         |       | %7.2f | %10.2f |\n",
           espera / n, retorno / n);
    printf("+------------------+-------+---------+------------+\n");
}

/* Verifica que haya procesos cargados a memoria (ready). Avisa de los que
   siguen en new y por eso se omiten. */
static int hay_procesos(void)
{
    int listos = 0, nuevos = 0;

    for (int i = 0; i < total; i++) {
        if (lista[i].estado == READY)
            listos++;
        else if (lista[i].estado == NEW)
            nuevos++;
    }

    if (total == 0) {
        printf("No hay procesos creados.\n");
        return 0;
    }

    if (listos == 0) {
        printf("No hay procesos en la ready queue. Use alloc <identificador> <estrategia> primero.\n");
        return 0;
    }

    if (nuevos > 0)
        printf("Aviso: %d proceso(s) en estado new no se ejecutan (falta alloc).\n",
               nuevos);

    return 1;
}

/* Copia los procesos en ready, en el orden en que llegaron a la ready queue
   (orden de alloc). Devuelve cuantos copio. */
static int preparar_copia(Proceso p[])
{
    int n = 0;

    for (int i = 0; i < total; i++) {
        if (lista[i].estado == READY) {
            p[n] = lista[i];
            p[n].restante = p[n].burst;
            n++;
        }
    }

    /* Ordenar por orden de llegada (insercion, estable) */
    for (int i = 1; i < n; i++) {
        Proceso aux = p[i];
        int j = i - 1;

        while (j >= 0 && p[j].orden > aux.orden) {
            p[j + 1] = p[j];
            j--;
        }

        p[j + 1] = aux;
    }

    return n;
}

/* Al terminar una simulacion, los procesos ejecutados pasan a terminated y
   dejan la memoria, pero siguen en la lista de procesos. */
static void finalizar_ejecutados(Proceso p[], int n)
{
    for (int k = 0; k < n; k++) {
        int idx = buscar_id(p[k].id);

        lista[idx].espera = p[k].espera;
        lista[idx].retorno = p[k].retorno;
        liberar_memoria(&lista[idx]);
        lista[idx].estado = TERMINATED;
    }

    printf("Los procesos ejecutados pasaron a terminated y salieron de memoria.\n");
}

/* FCFS: First-Come, First-Served */
static void fcfs(void)
{
    if (!hay_procesos())
        return;

    Proceso p[MAX_PROC];
    int tiempo = 0;
    int n = preparar_copia(p);

    printf("\n=== FCFS ===\n");

    for (int i = 0; i < n; i++) {
        printf("[t = %3d] Entra al CPU: proceso %s (ready -> running)\n",
               tiempo, p[i].id);

        p[i].espera = tiempo;
        tiempo += p[i].burst;
        p[i].retorno = tiempo;
        p[i].estado = TERMINATED;

        printf("[t = %3d] Sale del CPU: proceso %s (running -> terminated)\n",
               tiempo, p[i].id);
    }

    imprimir_tabla(p, n);
    finalizar_ejecutados(p, n);
}

/* SJF: Shortest-Job-First */
static void sjf(void)
{
    if (!hay_procesos())
        return;

    Proceso p[MAX_PROC];
    int tiempo = 0;
    int n = preparar_copia(p);

    /* Ordenar por burst menor a mayor (insercion, estable) */
    for (int i = 1; i < n; i++) {
        Proceso aux = p[i];
        int j = i - 1;

        while (j >= 0 && p[j].burst > aux.burst) {
            p[j + 1] = p[j];
            j--;
        }

        p[j + 1] = aux;
    }

    printf("\n=== SJF ===\n");

    for (int i = 0; i < n; i++) {
        printf("[t = %3d] Entra al CPU: proceso %s (ready -> running)\n",
               tiempo, p[i].id);

        p[i].espera = tiempo;
        tiempo += p[i].burst;
        p[i].retorno = tiempo;
        p[i].estado = TERMINATED;

        printf("[t = %3d] Sale del CPU: proceso %s (running -> terminated)\n",
               tiempo, p[i].id);
    }

    imprimir_tabla(p, n);
    finalizar_ejecutados(p, n);
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
    int n = preparar_copia(p);

    /* Llenar la cola en orden de llegada */
    for (int i = 0; i < n; i++) {
        cola[fin] = i;
        fin = (fin + 1) % MAX_PROC;
        cantidad++;
    }

    printf("\n=== Round Robin (Quantum = %d) ===\n", QUANTUM);

    while (cantidad > 0) {
        int i = cola[inicio];

        inicio = (inicio + 1) % MAX_PROC;
        cantidad--;

        printf("[t = %3d] Entra al CPU: proceso %s (ready -> running)\n",
               tiempo, p[i].id);

        /* Ejecutar un quantum o lo que le reste */
        int uso = (p[i].restante < QUANTUM) ? p[i].restante : QUANTUM;

        tiempo += uso;
        p[i].restante -= uso;

        if (p[i].restante > 0) {
            printf("[t = %3d] Sale del CPU: proceso %s (running -> ready, le faltan %d ut)\n",
                   tiempo, p[i].id, p[i].restante);

            /* Regresa al final de la cola */
            p[i].estado = READY;
            cola[fin] = i;
            fin = (fin + 1) % MAX_PROC;
            cantidad++;
        } else {
            printf("[t = %3d] Sale del CPU: proceso %s (running -> terminated)\n",
                   tiempo, p[i].id);

            p[i].estado = TERMINATED;
            p[i].retorno = tiempo;
            p[i].espera = tiempo - p[i].burst;
        }
    }

    imprimir_tabla(p, n);
    finalizar_ejecutados(p, n);
}

// manual de ayuda para el usuario
void mostrar_ayuda(void)
{
    printf(
        "\n"
        "=============== MANUAL DE mi_terminal ===============\n"
        "\n"
        "1. COMANDOS INTERNOS\n"
        "   help                 Muestra este manual.\n"
        "   exit                 Cierra el shell (tambien con Ctrl+D).\n"
        "   mkprocess, alloc, free, mstatus, compact, my_kill, lstprocss,\n"
        "   fcfs, sjf, rr   (ver seccion 5)\n"
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
        "5. SIMULACION DE PROCESOS, MEMORIA Y PLANIFICACION\n"
        "   mkprocess <identificador> <burst time> <tamano>\n"
        "       Crea un proceso en estado new (todavia no esta en memoria).\n"
        "         identificador  texto: letras, numeros, _ o - (max 16 caracteres),\n"
        "                        sin espacios; no puede repetirse (distingue\n"
        "                        mayusculas de minusculas). Ej: A, B, p1\n"
        "         burst time     tiempo en CPU (ut), 1-100000\n"
        "         tamano         bloques de memoria, 1-100000\n"
        "       Ejemplo: mkprocess A 25 8\n"
        "   alloc <identificador> <estrategia>\n"
        "       Carga el proceso a memoria (new -> ready) con asignacion\n"
        "       contigua. La memoria total es de %d bloques.\n"
        "         first   first fit: el primer hueco donde quepa\n"
        "         best    best fit: el hueco mas pequeno donde quepa\n"
        "         worst   worst fit: el hueco mas grande\n"
        "       Ejemplo: alloc A best\n"
        "       Falla si no hay un hueco contiguo suficiente (ver compact).\n"
        "       Un proceso terminated no puede volver a cargarse.\n"
        "   free <identificador>\n"
        "       Saca de memoria a un proceso en ready (vuelve a new).\n"
        "   mstatus\n"
        "       Muestra la memoria: procesos, huecos y fragmentacion externa.\n"
        "   compact\n"
        "       Compacta la memoria: junta los procesos al inicio y deja un\n"
        "       solo hueco libre.\n"
        "   my_kill <identificador>\n"
        "       Elimina el proceso de la lista; si estaba en ready tambien\n"
        "       libera su memoria.\n"
        "   lstprocss  (tambien lstprocess)\n"
        "       Lista los procesos con ID, burst, bloques, base en memoria\n"
        "       y estado.\n"
        "   fcfs\n"
        "       First-Come, First-Served: en el orden en que se hizo alloc.\n"
        "   sjf\n"
        "       Shortest-Job-First: ejecuta primero el de menor burst.\n"
        "   rr\n"
        "       Round-Robin con time quantum de %d ut.\n"
        "   fcfs, sjf y rr solo ejecutan los procesos en ready (cargados con\n"
        "   alloc). Muestran el orden de entrada y salida del CPU, el waiting\n"
        "   y turnaround de cada proceso y sus promedios. Al terminar, los\n"
        "   procesos pasan a terminated, salen de memoria y siguen en la\n"
        "   lista (max %d procesos); solo my_kill los elimina.\n"
        "   Estados: new -> ready (alloc) -> running -> terminated.\n"
        "   Los nombres de estos comandos aceptan mayusculas (FCFS, SJF, RR).\n"
        "\n"
        "6. ERRORES COMUNES\n"
        "   Comando vacio en tuberia      Falta un comando junto a '|'.\n"
        "   Error en execvp (x)           El programa 'x' no existe o no se\n"
        "                                 escribio con espacio (ej. grep.c).\n"
        "   Solo se admiten hasta N ...   Se excedio el maximo de pipas.\n"
        "   Ya existe un proceso ...      Identificador de mkprocess repetido.\n"
        "   No hay procesos en la ...     Use alloc <id> <estrategia> antes de fcfs/sjf/rr.\n"
        "   no hay un hueco contiguo ...  Hay fragmentacion: use compact.\n"
        "\n"
        "=====================================================\n"
        "\n",
        MAX_CMDS, MAX_CMDS - 1, MEM_BLOQUES, QUANTUM, MAX_PROC);
}

// comandos internos y bucle principal
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

    if (strcasecmp(cmd, "alloc") == 0) {
        alloc(a, na);
        return 1;
    }

    if (strcasecmp(cmd, "free") == 0) {
        cmd_free(a, na);
        return 1;
    }

    if (strcasecmp(cmd, "my_kill") == 0) {
        my_kill(a, na);
        return 1;
    }

    int es_sin_args = strcmp(cmd, "help") == 0 ||
                      strcasecmp(cmd, "lstprocss") == 0 ||
                      strcasecmp(cmd, "lstprocess") == 0 ||
                      strcasecmp(cmd, "mstatus") == 0 ||
                      strcasecmp(cmd, "compact") == 0 ||
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
    else if (strcasecmp(cmd, "mstatus") == 0)
        mstatus();
    else if (strcasecmp(cmd, "compact") == 0)
        compact();
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