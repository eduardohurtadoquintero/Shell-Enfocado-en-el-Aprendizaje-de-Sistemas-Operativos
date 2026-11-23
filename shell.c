
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <string.h>
#include <ctype.h>
 
#define MAX_LINEA 256
#define MAX_ARGS 32
#define MAX_CMDS 5   /* maximo de procesos conectados por pipas */
 
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
        "5. ERRORES COMUNES\n"
        "   Comando vacio en tuberia      Falta un comando junto a '|'.\n"
        "   Error en execvp (x)           El programa 'x' no existe o no se\n"
        "                                 escribio con espacio (ej. grep.c).\n"
        "   Solo se admiten hasta N ...   Se excedio el maximo de pipas.\n"
        "\n"
        "=====================================================\n"
        "\n",
        MAX_CMDS, MAX_CMDS - 1);
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
        if (n == 1 && strcmp(args[0][0], "exit") == 0)
            break;
 
        if (n == 1 && strcmp(args[0][0], "help") == 0) {
            mostrar_ayuda();
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