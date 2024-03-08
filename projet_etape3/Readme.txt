Le repertoire projet_etape3 est constituté de deux sous-repertoires :

**** /multithread : ***
Ce repertoire contient le fichier de la version du serveur multithreads.
----- Voici un exemple de sont exécution -------
gcc serveur_thread.c -o thread -lpthread // pour compiler le programme

./thread 8000 // pour exécuter le programme avec un port passé en argument

*** /select : ***
Ce repertoire contient le fichier de la version du serveur monothreadé.
----- Voici un exemple de sont exécution -------
gcc serveur_select.c -o select // pour compiler le programme

./select 9000 // pour exécuter le programme avec un port passé en argument