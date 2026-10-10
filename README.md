# Pilote Canon LASER SHOT LBP-1120 pour Windows x64

Pilote d'impression natif **x64** (modèle de pilote **v3**, Unidrv) pour l'imprimante laser Canon LASER SHOT LBP-1120. Cette imprimante est de type « host-based » : elle ne comprend aucun langage de page, l'ordinateur lui envoie directement le raster compressé dans le protocole propriétaire **CAPT**.

Le pilote est formé de deux DLL :

| DLL | Rôle |
|---|---|
| `lbp1120rnd.dll` | Plug-in de rendu Unidrv (`IPrintOemUni2`) : transforme le raster de chaque page en flux CAPT. |
| `lbp1120lm.dll` | Moniteur de langage **« LBP1120 CAPT Monitor »** : dialogue bidirectionnel CAPT avec l'imprimante. |

**Imprimante prise en charge**

- USB : `VID_04A9&PID_262B` (Canon) ;
- identifiant IEEE-1284 : `MFG:Canon;MDL:LASER SHOT LBP-1120;CMD:CAPT` ;
- identifiant matériel dans l'INF : `USBPRINT\CanonLASER_SHOT_LBP-91D2` (le nœud enfant USBPRINT créé par Windows). Une fois le paquet dans le magasin de pilotes, brancher l'imprimante peut donc sélectionner ce pilote.

**Système** : Windows 10 / Windows 11 x64.

## Installation

L'imprimante doit être branchée et allumée (le port `USB001` doit exister). L'installation se fait dans un **Windows PowerShell 5.1 lancé en administrateur** (`powershell.exe`, pas PowerShell 7).

### Depuis une release (sans compiler)

Aucun outil à installer.

1. Télécharger `lbp1120-driver-<version>-x64.zip` depuis la page **Releases** du dépôt GitHub et le décompresser.
2. Dans le Windows PowerShell administrateur, se placer dans le dossier décompressé `lbp1120-driver-<version>-x64`, puis :

   ```powershell
   Get-ChildItem -Recurse | Unblock-File; Set-ExecutionPolicy -Scope Process Bypass -Force; .\install.ps1 -EnableLog
   ```

   `Unblock-File` retire la marque « téléchargé depuis Internet » qui empêcherait l'exécution des scripts. `-EnableLog` est facultatif : il active le journal du moniteur (`%ProgramData%\LBP1120\lm.log`), utile en cas de problème.

### Depuis les sources

Prérequis : Visual Studio Build Tools avec l'ensemble d'outils MSVC x64 et le Windows SDK 10.0.26100 (détails dans [Prérequis](#prérequis)).

1. Compiler (une invite normale suffit) ; le paquet est préparé dans `pkg\` :

   ```powershell
   & 'C:\chemin\vers\lbp1120-driver\build.cmd'
   ```

2. Installer, dans le Windows PowerShell administrateur :

   ```powershell
   Set-ExecutionPolicy -Scope Process Bypass -Force; & 'C:\chemin\vers\lbp1120-driver\install.ps1' -EnableLog
   ```

### Page de test

```powershell
rundll32 printui.dll,PrintUIEntry /k /n "Canon LBP-1120 (CAPT x64)"
```

L'imprimante s'appelle **« Canon LBP-1120 (CAPT x64) »**. Autres paramètres d'`install.ps1` : `-PackageDir` (défaut `<dossier du script>\pkg`), `-PrinterName` (défaut `Canon LBP-1120 (CAPT x64)`), `-PortName` (défaut `USB001`).

Ce que fait `install.ps1` :

0. Copie le paquet dans un dossier neuf au nom aléatoire, `%SystemRoot%\Temp\lbp1120-<guid>`, créé d'emblée avec une DACL réservée à SYSTEM et aux Administrateurs, et affiche le SHA-256 de chaque fichier. Toutes les étapes suivantes travaillent sur ces copies, jamais sur le dossier d'origine modifiable par l'utilisateur.
1. Crée un certificat de signature de code auto-signé neuf (`CN=LBP1120 Test Driver Signing`) et ajoute sa partie publique, depuis la mémoire, à `LocalMachine\Root` et `LocalMachine\TrustedPublisher` (les anciennes copies sont retirées).
2. Construit le catalogue `lbp1120v3.cat` à partir de `lbp1120v3.cdf` avec les fonctions de `wintrust.dll` présentes dans tout Windows (celles qu'utilise `makecat`), signe le catalogue et les deux DLL avec `Set-AuthenticodeSignature` (SHA-256), puis **supprime la clé privée** : rien d'autre ne peut être signé avec ce certificat. Aucun outil du Windows SDK n'est nécessaire.
3. Ajoute le paquet au magasin de pilotes (`pnputil /add-driver`), installe le pilote d'imprimante `Canon LBP-1120 CAPT (x64)` et crée l'imprimante sur le port USB. L'INF copie `lbp1120lm.dll` dans `System32` et enregistre le moniteur « LBP1120 CAPT Monitor ». Le dossier temporaire est supprimé.

### Mettre à jour

Après avoir recompilé (`build.cmd`) ou décompressé une nouvelle release, dans le Windows PowerShell administrateur, depuis le dossier de la nouvelle version :

```powershell
Get-ChildItem -Recurse | Unblock-File; Set-ExecutionPolicy -Scope Process Bypass -Force; .\uninstall.ps1; .\install.ps1 -EnableLog
```

Le spouleur garde `lbp1120lm.dll` chargée : `uninstall.ps1` l'arrête le temps de supprimer le moniteur, et `install.ps1` vérifie (SHA-256) que `System32` contient bien la nouvelle DLL, sinon la remplace spouleur arrêté.

### Désinstaller

Dans le Windows PowerShell administrateur :

```powershell
Set-ExecutionPolicy -Scope Process Bypass -Force; & 'C:\chemin\vers\lbp1120-driver\uninstall.ps1'
```

Le script supprime les imprimantes qui utilisent le pilote, le pilote et son paquet dans le magasin de pilotes, le moniteur de langage, `System32\lbp1120lm.dll` et le certificat de test de `Root` et `TrustedPublisher`. Si le spouleur garde le moniteur chargé, le script l'arrête le temps de le supprimer, puis le redémarre.

## Architecture

```
Application
   │ GDI
   ▼
Unidrv (unidrv.dll, fourni avec Windows) + driver\lbp1120.gpd
   │ trame 1 bit/pixel, 600 x 600 ppp, A4 portrait, envoyée par bandes
   ▼
lbp1120rnd.dll  (plug-in de rendu, déclaré dans lbp1120.ini)
   │ IPrintOemUni2::ImageProcessing : assemble les bandes en une page, inverse la polarité GDI
   │ compression CAPT/SCoA (capt.c compress_bitmap) + séquence de page (capt.c print_page)
   │ IPrintOemDriverUni::DrvWriteSpoolBuf
   ▼
Spouleur (spoolsv.exe, processeur d'impression RAW)
   │ WritePort, morceaux de taille quelconque
   ▼
lbp1120lm.dll  (moniteur de langage, s'exécute en SYSTEM dans spoolsv.exe)
   │ CAPT bidirectionnel : réassemble les paquets, lit les réponses de l'imprimante,
   │ vérifie l'identité (a1 a1), attend le papier, attend que l'imprimante ne soit
   │ plus occupée (a0 e0) avant chaque paquet de données, envoie les données par blocs de 4096 octets
   ▼
usbmon.dll (port USB001) ─► usbprint.sys ─► USB ─► LBP-1120
```

**Plug-in de rendu.** Unidrv n'écrit lui-même aucun octet : toutes les commandes `*Cmd` du GPD sont vides et `DevBPP: 0` indique que `ImageProcessing` envoie le raster. Le plug-in place chaque bande à sa position dans la page (`IPPARAMS.ptOffset`), puis émet la page précédente au rappel `CmdStartPage` de la page suivante, ou dans le hook `DrvEndDoc` pour la dernière page. Un document annulé (`ED_ABORTDOC`) n'émet rien pour la page en cours. Géométrie : 4736 x 6968 pixels imprimables à l'origine (0,0) (A4 = 4960 x 7016 px), comme capt.c. Chaque commande d'état (`a1 a1`, `a0 a0`, `a1 a0`, `a0 e0`…) n'apparaît **qu'une fois** dans le flux ; les répétitions sont produites par le moniteur.

**Moniteur de langage.** Il se place entre le spouleur et `usbmon.dll`. Pour chaque paquet CAPT : écriture, pause de 8 ms, puis autant de lectures de réponse que l'exige l'opcode (comme capt.c). Il gère en plus :

- la vérification d'identité `a1 a1` (page 0) ; en cas d'échec, le travail échoue ;
- l'attente papier : tant que la réponse n'indique pas de papier, nouvel essai toutes les secondes, sans limite par défaut (`PaperWaitTries = 0`) et jusqu'à annulation du travail ; pendant l'attente, le port passe à l'état « plus de papier » (`SetPort` niveau 3, `PORT_STATUS_PAPER_OUT`) et la file affiche « Plus de papier : chargez du papier (LBP-1120) » ; l'impression reprend seule dès qu'une feuille est chargée ;
- une seconde vérification du papier juste avant les données de chaque page à partir de la deuxième (et de toute page dont la réponse `a0 e0` signale un bac vide, octet 5 bit `0x02`), une fois l'imprimante prête : la vérification de début de page de capt.c a lieu alors que la feuille précédente est encore dans le bac, et sans cette seconde vérification une page envoyée sans papier était perdue (constaté sur l'imprimante : 3 pages pour 2 feuilles) ; si, pendant cette attente, l'imprimante signale son erreur d'alimentation (réponse `a0 a0`, octet 5 bit `0x02`), elle a abandonné tout le travail (son état redevient celui d'avant un travail) : au retour du papier, le moniteur redémarre le travail avec la séquence d'initialisation de capt.c précédée de la commande `a2 e0` qu'envoie le pilote Canon d'origine (`a0 a2`, `a2 e0` … `a5 e0`), rouvre la page (`a0 d0` + paramètres, `a1 d0`) puis en envoie les données ;
- la porte « occupé » avant chaque paquet de données `a0 c0` : `a0 e0` toutes les 100 ms tant que l'imprimante est occupée, jusqu'à `GateTimeoutSec` ; après 3 s, la file affiche « Imprimante occupée : attente (LBP-1120) » ;
- l'annulation d'un travail (détectée en moins de 100 ms environ) et la fermeture du port ; un travail annulé depuis la file est arrêté sur l'imprimante comme le fait le pilote Canon d'origine, capt.c n'ayant rien pour cela : `a6 e0` (arrêt), `a0 a0` toutes les 500 ms jusqu'au repos de l'imprimante (au plus `GateTimeoutSec`), `a2 e0` si l'imprimante signale une attention, puis `a9 e0` (fin de travail) ;
- `JOB_CONTROL_LAST_PAGE_EJECTED` en fin de travail, pour que le travail quitte toujours la file.

Après une erreur (autre qu'une annulation), l'appel en cours renvoie `FALSE` une fois, puis le reste du travail est consommé sans rien envoyer à l'imprimante : le travail quitte la file au lieu de rester bloqué en « Erreur - Impression ».

## Contenu du dépôt

| Chemin | Contenu |
|---|---|
| `build.cmd` | Compile les deux DLL et prépare `pkg\`. |
| `install.ps1` | Signe (certificat de test) et installe le pilote, crée l'imprimante. |
| `uninstall.ps1` | Supprime tout ce qu'`install.ps1` a ajouté. |
| `package.ps1` | Construit le zip de release dans `dist\` à partir de `pkg\`. |
| `.github\workflows\release.yml` | Compilation et publication de la release par GitHub Actions. |
| `driver\lbp1120.gpd` | Description Unidrv : 600 x 600 ppp, 1 bpp, A4 portrait. |
| `driver\lbp1120.ini` | Déclare le plug-in (`OEMDriverFile1=lbp1120rnd.dll`). |
| `driver\lbp1120v3.inf` | INF x64 (`Include=NTPRINT.INF`, `Needs=UNIDRV.OEM,UNIDRV_DATA`, `LanguageMonitor=`). |
| `driver\lbp1120v3.cdf` | Définition du catalogue `lbp1120v3.cat`, construit et signé par `install.ps1`. |
| `src\render\capt_encode.*` | Encodeur CAPT et construction du flux de page (portage de capt.c). |
| `src\render\capt_page.*` | Assemblage des bandes Unidrv en une page (polarité PBM, 1 = noir). |
| `src\render\lbp1120rnd.*` | Plug-in COM, fabrique de classe, hooks `DrvStartDoc`/`DrvEndDoc`, exports, ressource de version. |
| `src\langmon\lbp1120lm.c/.def` | Moniteur de langage (exporte `InitializePrintMonitor2` et `LbpLmLogStatus`). |
| `COPYING` | Licence GPLv2. |

Produits de compilation (ignorés par git) : `obj\`, `bin\`, `pkg\`, `dist\`.

## Prérequis

- Windows 10/11 x64.
- Visual Studio Build Tools (testé avec la version 18) avec l'ensemble d'outils **MSVC x64** (composant `Microsoft.VisualStudio.Component.VC.Tools.x86.x64`).
- **Windows SDK 10.0.26100** : en-têtes `printoem.h`, `prcomoem.h`, `winddi.h` (compilation seulement).
- Pour l'installation : Windows PowerShell 5.1 (`powershell.exe`) lancé en administrateur ; aucun outil du SDK.

## Compilation

```
build.cmd
```

- `vcvarsall.bat` est trouvé avec `vswhere.exe` ; à défaut, `C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools` est utilisé. Pour forcer un autre chemin : `set VCVARS=<chemin complet de vcvarsall.bat>`. Lancé depuis une invite développeur x64, le script utilise l'environnement courant tel quel.
- Options : `/W4 /WX /O2 /MT /guard:cf /GS /Zi`, édition de liens `/DYNAMICBASE /NXCOMPAT /guard:cf`. CRT statique (`/MT`) : aucun redistribuable Visual C++ n'est nécessaire dans le spouleur. `lbp1120rnd.dll` est compilée avec `KERNEL_MODE`, `USERMODE_DRIVER`, `UNICODE` (comme l'exemple du WDK) ; `lbp1120lm.dll` n'importe que `KERNEL32.dll` et `ADVAPI32.dll`.
- Le script vérifie les exports (`DllGetClassObject`, `DllCanUnloadNow` ; `InitializePrintMonitor2`).
- Sorties : `obj\` (intermédiaires), `bin\lbp1120rnd.dll`, `bin\lbp1120lm.dll` (+ `.pdb`), et `pkg\`, le paquet non signé de 6 fichiers attendu par `install.ps1` : les 2 DLL et les 4 fichiers de `driver\`.
- Le script se termine par `BUILD OK` (code de sortie 0) ou `BUILD FAILED` (code 1). Chaque compilation repart de zéro (`obj\`, `bin\` et `pkg\` sont recréés).

## Publier une release

Le zip de release contient le paquet compilé (`pkg\`), `install.ps1`, `uninstall.ps1`, ce README et `COPYING` : l'utilisateur n'a rien à compiler ni à installer, la signature est faite sur sa machine par `install.ps1`.

- **Avec GitHub Actions** (`.github\workflows\release.yml`) : pousser un tag `v<version>` crée la release GitHub, avec le zip compilé sur un runner Windows de GitHub et son SHA-256 dans les notes. Lancé à la main (onglet Actions, workflow `release`, « Run workflow »), le workflow garde le zip comme artefact sans créer de release.

  ```powershell
  git tag v1.0; git push origin v1.0
  ```

- **En local** : `build.cmd`, puis `.\package.ps1 -Version 1.0`, qui écrit `dist\lbp1120-driver-1.0-x64.zip` et affiche son SHA-256, à joindre à une release créée à la main (`gh release create v1.0 dist\lbp1120-driver-1.0-x64.zip`).

Avant de publier une nouvelle version, mettre à jour son numéro dans `driver\lbp1120v3.inf` (`DriverVer`), `src\render\lbp1120rnd.rc` (`FILEVERSION`, `PRODUCTVERSION` et les chaînes) et `src\langmon\lbp1120lm.c` (`LM_VERSION`). Les binaires étant sous GPLv2, le code source doit les accompagner : GitHub joint automatiquement l'archive des sources du tag à chaque release.

## Paramètres

### Moniteur de langage

Valeurs directement sous `HKLM\SYSTEM\CurrentControlSet\Control\Print\Monitors\LBP1120 CAPT Monitor` (seuls les administrateurs peuvent écrire dans cette clé). Elles sont relues à chaque `StartDocPort` : elles s'appliquent dès le travail suivant, sans redémarrer le spouleur.

| Valeur | Type | Défaut | Signification |
|---|---|---|---|
| `LogLevel` | DWORD | **0 (désactivé)** | 0 = désactivé, 1 = journal du protocole, 2 = aussi chaque appel `WritePort` et les échecs de `SetJob` |
| `LogFile` | SZ/EXPAND_SZ | `%ProgramData%\LBP1120\lm.log` | Chemin complet obligatoire. Le dossier est créé avec une DACL protégée s'il n'existe pas. |
| `MaxLogKB` | DWORD | 4096 | Au-delà de cette taille, le journal est tronqué sur place (pas de rotation ni de renommage). |
| `CaptureSent` | DWORD | 0 | 1 = écrit chaque octet envoyé au port dans un nouveau fichier `<dossier du journal>\job<N>-<pid>-<tick>.sent.bin`. Nécessite `LogLevel` ≥ 1. |
| `ReadTimeoutMs` | DWORD | 2000 | Délai maximal de chaque lecture d'état |
| `WriteTimeoutMs` | DWORD | 30000 | Transmis à `SetPortTimeOuts` |
| `GateTimeoutSec` | DWORD | 120 | Attente maximale de l'imprimante prête avant un paquet de données |
| `PaperWaitTries` | DWORD | 0 | Nombre de vérifications du papier, à 1 s d'intervalle ; 0 = attendre sans limite (jusqu'à annulation) |
| `WriteRetries` | DWORD | 3 | Nouvelles tentatives pour un bloc de données dont rien n'a été écrit |

Exemple (administrateur) :

```powershell
Set-ItemProperty -Path 'HKLM:\SYSTEM\CurrentControlSet\Control\Print\Monitors\LBP1120 CAPT Monitor' -Name LogLevel -Type DWord -Value 1
```

### Plug-in de rendu (par imprimante)

Valeurs `REG_DWORD` facultatives dans les données du pilote de l'imprimante (`HKLM\SYSTEM\CurrentControlSet\Control\Print\Printers\<imprimante>\PrinterDriverData`), lues par `GetPrinterData` à `EnablePDEV`. Les valeurs par défaut reproduisent le comportement validé.

| Valeur | Défaut | Signification |
|---|---|---|
| `CaptTopSkip` | 0 | Lignes sautées en haut de page. Le contenu remonte et les `CaptTopSkip` dernières lignes (sur 6968) restent blanches. |
| `CaptLeftSkip` | 0 | Pixels sautés à gauche (seul `/8` est utilisé, comme capt.c). |
| `CaptPolarity` | 0 | 0 = automatique, 1 = inverser, 2 = ne pas inverser (à utiliser si la page sort en négatif). |
| `CaptBandOrder` | 0 | 0 = bandes de haut en bas, 1 = de bas en haut. |

Exemple (administrateur) :

```powershell
reg add "HKLM\SYSTEM\CurrentControlSet\Control\Print\Printers\Canon LBP-1120 (CAPT x64)\PrinterDriverData" /v CaptPolarity /t REG_DWORD /d 2
```

Le spouleur met ces données en cache : le redémarrer si la valeur n'est pas prise en compte.

## Journalisation

**Moniteur de langage.** Désactivé par défaut. `install.ps1 -EnableLog` ou `LogLevel=1` l'active ; le fichier est `%ProgramData%\LBP1120\lm.log`. Exemple de lignes :

```
[     0.001] USB001 j100 w: n=4 : a1 a1 04 00
[     0.010] USB001 j100 r: n=6 : a1 a1 14 00 00 00
[     0.652] USB001 j100 w: n=4096/4096  (i=0/10377)
[     3.496] USB001 j100 ! FAIL: Timeout out while waiting for paper. (error 28)
[     0.836] USB001 j100 DONE (EndDocPort): 23 commands (0 gate polls), 1 data packets / 3 data writes, 10511 bytes in, 10511 bytes sent, 1502 WritePort calls
```

Pour le lire (le dossier n'est accessible qu'à SYSTEM et aux Administrateurs), depuis un PowerShell administrateur :

```powershell
Get-Content "$env:ProgramData\LBP1120\lm.log" -Tail 50
```

Ou pour le copier vers un emplacement lisible par l'utilisateur :

```powershell
Copy-Item "$env:ProgramData\LBP1120\lm.log" "$env:USERPROFILE\Desktop\lm.log"
```

Règles de sécurité : le moniteur s'exécute en SYSTEM et n'importe quel utilisateur peut créer un dossier sous `%ProgramData%`. Pour qu'une jonction, un lien symbolique ou un lien physique ne puisse pas détourner l'écriture vers un autre fichier :

- journal et capture sont **désactivés par défaut** et seul un administrateur peut les activer (clé HKLM ci-dessus) ;
- le spouleur appelle le moniteur en empruntant l'identité de l'utilisateur qui imprime : dossier et fichiers sont donc créés par un thread dédié du moniteur, qui s'exécute sous l'identité du service (SYSTEM) et non sous celle de cet utilisateur ;
- un dossier absent est créé avec la DACL `D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)` (SYSTEM et Administrateurs uniquement) ;
- le dossier est refusé s'il s'agit d'un point d'analyse (jonction), si son propriétaire n'est pas SYSTEM, Administrateurs ou l'utilisateur du processus, ou si son chemin final diffère du chemin configuré (jonction dans un dossier parent) ; il reste ouvert sans `FILE_SHARE_DELETE` pendant la journalisation ;
- un fichier est refusé s'il est un point d'analyse ou a plus d'un lien physique ; les captures sont créées avec `CREATE_NEW` et un nom unique ; aucun `MoveFileEx` ;
- **tout contrôle en échec désactive la journalisation** : le moniteur ne répare rien, il note la raison (export `LbpLmLogStatus` et `OutputDebugString`) et revérifie au travail suivant.

**Plug-in de rendu.** Il écrit via `OutputDebugString` (préfixe `lbp1120rnd:`), visible avec Sysinternals DebugView (« Capture Global Win32 », en administrateur) : paramètres, décision de polarité, et par page `raster WxH bytes from N band(s) -> 67 CAPT bands, S stream bytes`, ainsi que les erreurs.

## Dépannage

- **Aucun travail visible / rien ne sort.** Vérifier que l'imprimante est allumée et que le port existe (`Get-PrinterPort USB001`), puis activer le journal (`LogLevel=1`) et relancer l'impression. Si `lm.log` contient `! FAIL:`, le travail a échoué et a été vidé de la file (voir les codes ci-dessous). Si `lm.log` n'est pas créé, soit la journalisation a été refusée, soit le moniteur n'a pas été appelé : vérifier que la clé `HKLM\SYSTEM\CurrentControlSet\Control\Print\Monitors\LBP1120 CAPT Monitor` existe et que la prise en charge bidirectionnelle est activée sur le port (par défaut pour les ports USB). Pour le plug-in, regarder la sortie DebugView.
- **Page de test.** La méthode WMI `PrintTestPage` (`Invoke-CimMethod` sur `Win32_Printer`) renvoie 5 (accès refusé) hors session élevée. Utiliser `rundll32 printui.dll,PrintUIEntry /k /n "Canon LBP-1120 (CAPT x64)"`.
- **Éteindre et rallumer l'imprimante** si un travail s'est arrêté en cours de route sur une erreur ou un débranchement, avant de réessayer : la page en cours peut rester ouverte côté imprimante, ce que le journal signale. Un travail annulé depuis la file est, lui, arrêté proprement sur l'imprimante.
- **Page en négatif** : `CaptPolarity`. **Décalage** : `CaptTopSkip` / `CaptLeftSkip`.

Codes d'erreur du moniteur (dans `lm.log` et le statut du travail) :

| Situation | Erreur |
|---|---|
| Travail annulé ou port fermé | `ERROR_PRINT_CANCELLED` (63) |
| `a1 a1` sans réponse ou incorrect (« Invalid printer state, printer not connected ? ») | `ERROR_NOT_READY` (21) |
| Pas de papier après `PaperWaitTries` (si différent de 0) | `ERROR_OUT_OF_PAPER` (28) |
| Imprimante jamais prête après `GateTimeoutSec` | `ERROR_TIMEOUT` (1460) |
| Écriture en échec (imprimante débranchée…) | erreur d'usbmon (par exemple 1167) ou `ERROR_WRITE_FAULT` |
| Flux désynchronisé | `ERROR_INVALID_DATA` |

## État

**Vérifié sur l'imprimante réelle** (pilote installé avec `install.ps1`) :

- page de test Windows imprimée correctement ;
- format A4 ;
- travail d'une seule page ;
- travail de plusieurs pages : les pages s'enchaînent ;
- manque de papier en cours de travail (3 pages pour 2 feuilles) : attente, puis reprise automatique au chargement d'une feuille, la 3ᵉ page est imprimée ;
- annulation d'un travail depuis la file : l'imprimante termine la feuille déjà engagée puis s'arrête. Une fois, la page de test imprimée juste après est sortie blanche (la suivante était correcte) ; cause non identifiée.

**Vérifié hors ligne** (bancs de test du dépôt de développement, non inclus ici) : le flux produit par `lbp1120rnd.dll` piloté comme par Unidrv est identique octet pour octet au flux de référence (une et deux pages, document annulé) ; le moniteur, testé contre un simulateur d'imprimante, produit le même trafic que l'outil de référence, y compris avec attente « occupé », attente papier, redémarrage du travail après une erreur d'alimentation, arrêt d'un travail annulé et lectures sans réponse.

**Pas encore vérifié sur l'imprimante réelle** :

- débranchement pendant un travail (le premier point à contrôler : le travail doit échouer en `ReadTimeoutMs` au lieu de bloquer le port) ;
- format Letter (le GPD ne propose actuellement que A4).

## Limites

- **Signature de test uniquement** : le paquet est signé par un certificat auto-signé de confiance sur la machine, pas par Microsoft. Il ne peut pas être distribué tel quel. Si Windows refuse malgré tout le paquet, le mode test (`bcdedit /set testsigning on`, redémarrage) peut être nécessaire.
- **Windows Protected Print Mode** (Windows 11 24H2 et suivants) : s'il est activé, il bloque les pilotes tiers v3 et les moniteurs de langage tiers ; ce pilote ne fonctionne pas dans ce mode.
- **Modèle de pilote v3** : Microsoft retire progressivement les pilotes d'impression tiers v3 au profit du pilote IPP de classe ; ce pilote pourrait cesser de fonctionner dans une future version de Windows.
- A4 portrait uniquement, 600 ppp uniquement ; pas d'orientation paysage proposée ; les copies sont re-rendues page par page (`*MaxCopies: 1`).
- Pas de schéma bidirectionnel (`SendRecvBidiDataFromPort` n'est pas implémenté).

## Licence et crédits

Licence **GNU GPL version 2 ou ultérieure** (voir `COPYING`).

L'encodeur CAPT (`src\render\capt_encode.*`) et la logique du protocole du moniteur de langage sont dérivés de **capt-0.1** de Nicolas Boichat (`capt.c`, © 2004), qui contient des parties © 2000 Rildo Pragana. Les deux DLL sont donc sous GPLv2.

Canon et LASER SHOT sont des marques de Canon Inc. Ce pilote est un projet indépendant, sans lien avec Canon, ni approuvé ni pris en charge par Canon.
