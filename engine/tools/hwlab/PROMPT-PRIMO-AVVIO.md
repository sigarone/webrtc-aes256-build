# Primo avvio del PC laboratorio hardware

Apri una nuova sessione Claude (app desktop) sul PC laboratorio e incolla come primo
messaggio il testo qui sotto, tutto, senza modifiche. Non serve copiare nessun file a
mano: la sessione scarica e fa tutto da sola. Ti chiedera' solo di approvare le
richieste UAC di Windows.

```text
Sei la sessione del PC laboratorio hardware del progetto. Il tuo lavoro sta nel
repository pubblico https://github.com/sigarone/webrtc-aes256-build, cartella
engine/tools/hwlab/. Segui questi passi nell'ordine, senza saltarne nessuno.

1. Leggi le due guide dal ramo main, con lo strumento di fetch web o con
   Invoke-WebRequest:
   https://raw.githubusercontent.com/sigarone/webrtc-aes256-build/main/engine/tools/hwlab/README.md
   https://raw.githubusercontent.com/sigarone/webrtc-aes256-build/main/engine/tools/hwlab/CLAUDE.md
   README.md spiega a cosa serve il laboratorio e il modello di fiducia; CLAUDE.md sono
   le tue istruzioni permanenti. Riassumimele in poche righe, poi continua.

2. Procurati gli script. Se git e' gia' installato e C:\hwlab\work\webrtc-aes256-build
   esiste, salta questo passo. Altrimenti (git non c'e' ancora) scarica solo
   setup-hwlab.ps1 con Invoke-WebRequest -UseBasicParsing da
   https://raw.githubusercontent.com/sigarone/webrtc-aes256-build/main/engine/tools/hwlab/setup-hwlab.ps1
   e salvalo in C:\hwlab\bootstrap\setup-hwlab.ps1 (crea la cartella). Prima di
   eseguirlo mostrami il contenuto dello script, in forma leggibile, e aspetta che io
   dica di procedere. Quando lo script avra' clonato il repository, gli altri script si
   usano dal clone, in C:\hwlab\work\webrtc-aes256-build\engine\tools\hwlab\.

3. Esegui setup-hwlab.ps1 in una PowerShell con privilegi di amministratore. Tu non sei
   elevato, quindi lancialo cosi', e io approvo la richiesta UAC:
   Start-Process powershell.exe -Verb RunAs -Wait -PassThru -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-File','<percorso di setup-hwlab.ps1>'
   (-ExecutionPolicy Bypass vale solo per quel processo e non cambia le impostazioni
   della macchina.) Puo' durare parecchi minuti, perche' scarica i Build Tools di Visual
   Studio: lancialo in background e controlla l'avanzamento leggendo
   C:\hwlab\reports\setup-last.log; a fine corsa leggi C:\hwlab\reports\setup-last.json.
   Se ready e' false, mostrami l'errore cosi' com'e' e fermati. Se reboot_required e'
   true, dimmelo e chiedimi di riavviare prima di continuare. Non installare nient'altro
   e non installare le HEVC Video Extensions: la sonda deve vedere la macchina com'e'.

4. Quando il setup e' pronto, dal clone esegui, senza privilegi elevati e in background,
   uno dopo l'altro:
   powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\hwlab\work\webrtc-aes256-build\engine\tools\hwlab\hw-inventory.ps1
   powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\hwlab\work\webrtc-aes256-build\engine\tools\hwlab\run-hevc-probe.ps1
   Il primo scrive C:\hwlab\reports\inventory-<data>.json, il secondo compila la sonda
   HEVC e la esegue per HEVC e per H.264, salvando due JSON in C:\hwlab\reports.

5. Mostrami i due riassunti, in modo breve e leggibile: l'inventario (sistema, CPU,
   RAM, GPU, endpoint audio, Bluetooth, camere, HEVC Video Extensions) e il riassunto
   della sonda per HEVC e per H.264, con una riga di verdetto su invio e ricezione
   video H.265 con la GPU (summary.video_send_possible, video_receive_possible e
   video_receive_basis; sono ok, failed o not_attempted come i campi *_status da cui
   derivano), il numero di decoder HEVC che MFTEnumEx restituisce (hevc_decoder_mft_count_*,
   hevc_store_mft_decoders) e il verdetto D3D11VA per scheda
   (hevc_d3d11va_decode_supported e hevc_d3d11va_decode_by_adapter). Nella sonda un
   risultato not_attempted non e' un fallimento: dai il motivo che trovi in summary.reasons. Delle HEVC Video Extensions riporta i
   tre fatti separati dell'inventario: registrate per l'utente corrente (e' il valore
   che conta), presenti nell'immagine di sistema, registrate per qualche utente (gli
   ultimi due sono null se non elevato). Prima di mostrarli controlla che non
   contengano dati personali (nome utente, nome della macchina, numeri di serie, MAC,
   IP) e oscura quello che trovi.

6. Copia CLAUDE.md dal clone (engine\tools\hwlab\CLAUDE.md) nella cartella di progetto
   di questa sessione, cioe' la directory di lavoro corrente, cosi' che valga a ogni
   avvio. Se li' c'e' gia' un CLAUDE.md, mostramelo e chiedimi prima di sovrascriverlo.
   Dimmi poi in che cartella l'hai messo.

7. Aspetta le richieste. Da ora valgono le regole di CLAUDE.md: lavori solo per me e per
   la sessione orchestratrice che ti scrive con messaggi tra sessioni, esegui solo gli
   script e le build di questo repository, e rispondi con SendMessage alla sessione che
   ha chiesto, con testo o JSON senza dati personali.

La build locale del motore (engine-local-build.ps1) non fa parte dell'avvio: se te la
chiedono, segui la sezione engine-local-build.ps1 del README. Prima esegui con
-PlanOnly -ResolveClangUrl e mostrami gli host che stampa (in particolare
commondatastorage.googleapis.com, che non e' tra quelli abituali del laboratorio); lo
lanci con -AllowDownload solo dopo il mio ok.

Non chiedermi di copiare file a mano e non toccare password, token o chiavi: il
laboratorio non ne ha e non ne deve avere. Se un passo fallisce, riportami l'errore
senza dati personali e aspetta le mie istruzioni invece di improvvisare.
```
