# Mini Système de Fichiers (mini_fs)

## Introduction

Ce projet implémente un système de fichiers en mémoire complet avec :
- Opérations sur les fichiers et répertoires
- Gestion des permissions
- Liens symboliques et physiques
- Système de pagination pour le stockage
- Support multi-utilisateurs
- Fonctionnalités de sauvegarde/restauration

## Compilation du projet

Pour compiler le programme, utilisez le Makefile fourni :

```bash
make
```

Ceci créera l'exécutable `mini_fs`.

## Utilisation

Lancez le gestionnaire de système de fichiers avec :

```bash
./mini_fs
```

### Commandes disponibles

Une fois dans le shell interactif, vous pouvez utiliser la commande ```help()``` pour afficher un guide systeme

## Dépannage

### Problèmes de compilation
- Assurez-vous d'avoir les dépendances requises :
  ```bash
  sudo apt install build-essential
  ```
- Si vous obtenez des erreurs pthread, essayez :
  ```bash
  make clean
  make
  ```

### Problèmes d'exécution
- Erreurs "Permission denied" : Vérifiez les permissions et le propriétaire
- "Fichier non trouvé" : Vérifiez le chemin et le répertoire courant
- "Répertoire non vide" : Supprimez le contenu avant de supprimer le répertoire

## Détails d'implémentation

- Utilise un stockage en mémoire avec pagination simulée
- Supporte plusieurs utilisateurs avec authentification
- Implémente la résolution complète des chemins
- Gère les métadonnées des fichiers incluant :
  - Les permissions
  - La propriété
  - Les horodatages
  - Le comptage des liens

## Améliorations futures

- Stockage persistant sur disque
- Support des attributs étendus
- Compression des fichiers
- Capacités de partage réseau

## Licence

Ce projet est publié sous licence MIT.
