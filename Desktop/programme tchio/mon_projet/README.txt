

### README.txt - Projet Minishell

**Auteurs :
 Franck Tchio
 Serge Yankou
**Formation :** ET3 - Polytech Paris-Saclay 
**Sujet :** Implémentation d'un interpréteur de commandes Unix simplifié en C.

---

#### 1. Présentation du Projet
Ce projet consiste en la création de `minishell`, un interpréteur de commandes capable d'exécuter des programmes système, de gérer les entrées/sorties et d'assurer le contrôle des processus (jobs)
Le développement a été structuré en 5 étapes progressives allant du simple parseur au job control complet.

#### 2. Fonctionnalités Implémentées
Toutes les parties du sujet sont **opérationnelles** :

Boucle REPL :** Affichage du prompt, lecture de ligne avec `fgets()` et gestion du `EOF` (Ctrl-D)
Exécution :** Création de processus via `fork()`, remplacement d'image avec `execvp()` et attente avec `waitpid()`
Builtins :** Implémentation des commandes internes `cd` (avec gestion du `$HOME`), `exit`, `jobs`, `fg` et `bg`
Redirections :** Gestion des opérateurs `<`, `>`, et `>>` via `dup2()` et `open()`.
Pipes :** Support des pipes multiples (ex: `ls | grep .c | wc -l`) via une fonction `execute_pipe` récursive.
Signaux :** Gestion de `SIGINT` (Ctrl-C) et `SIGTSTP` (Ctrl-Z) pour protéger le shell tout en contrôlant les fils.
Job Control :** Gestion d'une liste dynamique de jobs, lancement en arrière-plan (`&`) et nettoyage des processus zombies avec `WNOHANG`.

#### 3. Difficultés Rencontrées
Nous avons rencontré des défis techniques majeurs, notamment sur :

Redirections et Pipes :** La gestion des flux avec `dup2()` et la synchronisation des pipes (fermeture des descripteurs inutiles) ont été complexes à stabiliser.
Gestion des Groupes :** L'utilisation de `setpgid()` et `tcsetpgrp()` pour donner ou reprendre le contrôle du terminal a demandé une attention particulière pour éviter les blocages du shell.

Malgré ces difficultés, l'ensemble des fonctionnalités donne des résultats conformes aux tests attendus.


#### 4. Compilation et Lancement
```bash
gcc -Wall -Wextra -o minishell minishell.c
./minishell
`

