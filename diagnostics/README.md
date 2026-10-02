# Suita de diagnostic Scantailor-DGI

Scripturi PowerShell care măsoară cum se comportă Scantailor-DGI pe un anumit
calculator: cât stă interfața blocată și în ce operație, cât durează fiecare
etapă de procesare, cât de repede se încarcă paginile, dacă memoria sau
handle-urile cresc de la un ciclu la altul și cât de rapid este discul sau
share-ul de rețea pe care lucrează aplicația.

Audiență: administratorul IT (rulează testele pe PC-urile operatorilor) și
dezvoltatorul (citește rapoartele și compară PC-urile).

## Ce face și ce NU dovedește

**Ce face.** Măsoară. Aplicația scrie în timp ce rulează un jurnal de
diagnostic (`scantailor-perf-*.jsonl`, formatul este descris în `SCHEMA.md`),
iar scripturile îl transformă într-un raport care arată unde se duce timpul:
ce operație era pe firul interfeței când aceasta nu mai răspundea, ce etapă de
procesare e lentă, cât de lent e discul.

**Ce NU dovedește.**

- Un test curat pe un PC **nu** dovedește că alt PC este în regulă. PC-urile
  diferă prin disc, antivirus, rețea, drivere, setări. De aceea testul se
  rulează pe fiecare PC în parte, iar rapoartele se compară.
- Un test curat nu garantează absența problemelor: arată doar că, în timpul
  acelui test, nu s-a depășit niciun prag.
- Verdictele sunt praguri, nu diagnostice. Un WARN înseamnă „merită privit",
  nu „e defect".

**Blocajele cauzate de un disc lent sau de un share de rețea sunt constatări
reale, nu artefacte ale testului.** Operatorul le simte exact la fel. Dacă
raportul arată că interfața a stat 3 secunde într-o salvare pe share, asta
este o problemă reală de rezolvat (în aplicație sau în infrastructură).

## Fișiere

| Fișier | Rol |
|---|---|
| `Run-StressTest.cmd` / `.ps1` | Test de stres: rulează scenariul automat al aplicației și produce raportul |
| `Collect-Diagnostics.cmd` / `.ps1` | Strânge jurnalele „pasive" (de zi cu zi) de pe un PC, plus inventarul, într-un zip |
| `Analyze-Diagnostics.cmd` / `.ps1` | Face raportul (`report.html`, `summary.json`, `summary.md`) din orice folder/zip cu jurnale |
| `Compare-Reports.cmd` / `.ps1` | Pune două sau mai multe rapoarte unul lângă altul |
| `DiagnosticsCommon.psm1` | Funcții comune (citire jurnal, inventar PC, test de stocare, grafice) |
| `SCHEMA.md` | Formatul jurnalului de diagnostic (în engleză) |

## Cerințe

- Windows 10 sau 11, Windows PowerShell 5.1 (este inclus în Windows). Merge și
  cu PowerShell 7.
- **Nu** sunt necesare drepturi de administrator. Fără ele, unele informații
  (de exemplu excepțiile Windows Defender) apar ca „nu se pot citi"; restul
  funcționează.
- Nu e nevoie de Python sau de alte programe.
- Pornește scripturile **prin fișierele `.cmd`** (dublu-clic sau din linia de
  comandă). Acestea ocolesc politica de execuție (ExecutionPolicy) doar pentru
  procesul respectiv, fără să schimbe vreo setare a calculatorului, și
  deblochează fișierele descărcate de pe internet („Mark of the Web").
- Dacă politica de execuție este impusă prin Group Policy (GPO), nici `.cmd`-ul
  nu o poate ocoli; atunci administratorul trebuie să permită scripturile din
  acest folder.
- Folderul `diagnostics` se pune lângă `scantailor.exe` (de exemplu
  `C:\Program Files\Scantailor-DGI\diagnostics\`), sau se indică aplicația cu
  `-Exe`.

Căi: folosește, pe cât posibil, căi fără diacritice și fără spații ciudate
(de exemplu `D:\stress`). Scripturile avertizează dacă o cale are caractere
non-ASCII.

## 1. Test rapid (câteva minute)

Verifică doar că totul merge, cu pagini generate automat:

```
Run-StressTest.cmd -Quick
```

La final se deschide `report.html`. Rezultatele stau în
`%LOCALAPPDATA%\scantailor-dgi-stress\<data-ora>\`.

Un test complet cu pagini generate (40 de pagini, 5 cicluri):

```
Run-StressTest.cmd -Synthetic 40 -Label "PC Milena"
```

Pentru verdictul de „scurgere de memorie" sunt necesare cel puțin 5 cicluri
(primul este considerat încălzire și nu se folosește).

## 2. Test realist, cu scanări reale

Alege un titlu tipic (de exemplu 40 de pagini). **Nu indica niciodată cu
`-Scans` un folder de ieșire din producție.** Aplicația doar citește scanările;
proiectul și paginile procesate se scriu numai în folderul testului.

Varianta A – scanările copiate întâi local (măsoară PC-ul, nu rețeaua):

```
Run-StressTest.cmd -Scans "\\nas\titluri\T123\scans" -Pages 40 -CopyScansLocal -Label "PC Milena local"
```

Varianta B – scanările citite direct de pe share (cum lucrează operatorii):

```
Run-StressTest.cmd -Scans "\\nas\titluri\T123\scans" -Pages 40 -ProbeDir "\\nas\titluri" -Label "PC Milena share"
```

`-ProbeDir` face și un test de viteză al share-ului (scriere de fișiere mici,
scriere sigură de 1 MB, redenumire, citire). Testul lucrează într-un subfolder
temporar `st-diag-probe-xxxxxxxx`, pe care încearcă să-l șteargă la final.
Dacă nu reușește (de exemplu pe un share care permite crearea de fișiere, dar
nu și ștergerea lor), consola și raportul arată calea folderului rămas ca
eroare („cleanup: could not remove the probe folder ...”), iar folderul
trebuie șters manual. Pentru folderul cu scanări se face doar test de citire.

Compară apoi cele două rapoarte (secțiunea 5): diferența dintre A și B arată
cât costă rețeaua.

Cu setările reale ale operatorului (se folosește o **copie** a fișierului de
setări; originalul nu se modifică):

```
Run-StressTest.cmd -Scans "D:\stress\scans" -UseOperatorSettings -Label "PC Dani, setările lui"
```

Pe PC-uri lente sau cu multe pagini, mărește timpul maxim (implicit 60 de
minute): `-TimeoutMinutes 180`.

## 3. Trei instanțe simultan (ca operatorii)

```
Run-StressTest.cmd -Synthetic 40 -Instances 3 -TimeoutMinutes 180 -Label "PC Dani x3"
```

Fiecare instanță are propriul folder (`inst1`, `inst2`, `inst3`) și toate
pornesc în același timp, deci concurează pentru procesor, memorie și disc.
Raportul are o secțiune de comparație între instanțe.

## 4. Jurnalele de zi cu zi (colectare după o zi de lucru)

Aplicația scrie în mod normal un jurnal de diagnostic „de bază" (costă foarte
puțin): blocajele interfeței, operațiile lente și un eșantion de resurse la
fiecare 30 de secunde. La sfârșitul zilei, pe PC-ul operatorului:

```
Collect-Diagnostics.cmd -Label "PC Milena"
```

Scriptul caută jurnalele în:

- `%LOCALAPPDATA%\scantailor-dgi\scantailor-dgi\crashes` (versiunea instalată);
- `<folderul aplicației>\config\crashes` (versiunea portabilă; se poate indica
  cu `-AppDir "D:\Scantailor-DGI"`);
- `%TEMP%\scantailor-crashes` (rezervă, când celelalte nu se pot scrie).

Copiază jurnalele din ultimele 7 zile (`-Days 3` pentru mai puține), notele de
crash și, doar cu `-IncludeDumps`, fișierele `.dmp` (pot fi mari). Adaugă
inventarul PC-ului, face raportul și creează pe Desktop
`diag-<CALCULATOR>-<data-ora>.zip`. Acesta este fișierul de trimis
dezvoltatorului. În folderele aplicației nu se șterge și nu se modifică nimic.

Cu test de viteză al share-ului: `Collect-Diagnostics.cmd -ProbeDir "\\nas\titluri"`.

Dacă Scantailor rulează în timpul colectării, jurnalul curent este copiat așa
cum este în acel moment (raportul va arăta sesiunea fără înregistrarea de
oprire, ceea ce este normal în acest caz).

## 5. Compararea a două PC-uri

```
Compare-Reports.cmd "C:\...\scantailor-dgi-stress\20260923-101500" "C:\...\scantailor-dgi-stress\20260923-143000"
Compare-Reports.cmd diag-PC-DANI-20260923-1700.zip diag-PC-MILENA-20260923-1705.zip
```

Argumentele pot fi foldere de test, foldere de colectare, zip-uri făcute de
`Collect-Diagnostics` sau direct fișiere `summary.json`. Rezultatul
(`compare-<data-ora>.html` pe Desktop, sau `-Out cale.html`) pune valorile
alături: rândurile în care mediul diferă sunt galbene, iar la numere valoarea
cea mai proastă este roșie (când e de peste 1,5 ori mai mare decât cea mai
bună) și cea mai bună verde. Procentul de timp blocat și blocajele pe oră se
calculează pe timpul de rulare al tuturor proceselor, inclusiv al celor fără
niciun blocaj. Pentru un raport făcut cu o versiune mai veche a analizei pe un
test în modul `reopen`, viteza etapei Output este omisă (includea verificările
ieșirii deja făcute); rulează din nou `Analyze-Diagnostics.cmd` pe acel folder.

## Cum citești raportul

Sus este caseta de verdict. Fiecare rând are un status și cifrele din spatele
lui; sub cifre este scris pragul folosit.

- **PASS** – nu s-a depășit niciun prag.
- **WARN** – merită privit.
- **FAIL** – problemă clară în timpul testului.
- **N/A** – nu se poate spune (de exemplu prea puține cicluri).

Verdictele:

- **GUI responsiveness (reactivitatea interfeței).** Un „blocaj" (stall)
  înseamnă că firul interfeței nu a răspuns la un semnal trimis la fiecare
  50 ms timp de cel puțin 250 ms: fereastra era înghețată. WARN dacă un blocaj
  a depășit 1 s sau dacă blocajele însumează peste 1% din timpul de rulare;
  FAIL dacă un blocaj a depășit 5 s. Blocajele de la pornire și închidere și
  cele din interiorul pașilor testului (`stress.*`) apar în tabel, dar nu intră
  în verdict. Tabelul „Which operation the GUI thread was in" arată în ce
  operație era interfața – de aici se vede cauza (de exemplu
  `file.atomic_commit.sync` = așteptare după disc/rețea la salvare).
  Cât timp un blocaj durează, aplicația scrie la fiecare 10 s o înregistrare
  de progres (`stall_progress`). Dacă jurnalul se termină în timpul unui
  blocaj (aplicația era înghețată când a căzut sau a fost oprită), raportul îl
  arată ca „log ends during a GUI stall of >= X”, cu operația și stiva de
  apeluri: blocajul intră în verdict cu durata atinsă (a durat cel puțin
  atât; FAIL de la 5 s) și explică lipsa înregistrării de oprire sau
  depășirea timpului maxim.
- **Growth across cycles (creștere de la un ciclu la altul)** pentru memorie
  privată, handle-uri, obiecte GDI/USER și fire de execuție. La finalul fiecărui
  ciclu aplicația închide proiectul, se liniștește și ia un eșantion. Se trage
  o dreaptă prin eșantioane (fără primul ciclu). WARN pentru memorie: peste
  2 MB/ciclu cu r² peste 0,6; FAIL: peste 10 MB/ciclu cu r² peste 0,8 (r²
  aproape de 1 = creștere constantă, aproape de 0 = zgomot). Pragurile pentru
  handle-uri: 20/ciclu, GDI 10, USER 10, fire 2 (FAIL la de 5 ori mai mult).
  Sunt necesare cel puțin 4 cicluri după cel de încălzire.
- **Crashes and exit codes.** FAIL la orice crash (inclusiv fișiere
  `scantailor-*.txt/.dmp`), ieșire cu cod de eroare, depășirea timpului maxim,
  scenariu neterminat, lipsă de memorie, sau o instanță a testului care nu a
  putut porni ori nu a scris niciun jurnal (apare cu motivul, din `run.json`).
  O instanță oprită chiar de script pentru că testul a fost întrerupt (Ctrl+C
  sau o eroare a scriptului) este WARN, nu crash; raportul are atunci sus
  mențiunea „Run aborted before completion - results are partial”. În
  jurnalele de zi cu zi, o sesiune fără înregistrare de oprire este WARN (poate
  fi și o închidere forțată sau o pană de curent).
- **Task failures.** FAIL dacă o sarcină de procesare s-a terminat cu
  `bad_alloc`, `exception` sau `unknown`, sau dacă testul a înregistrat un eșec.
  La nivelul `basic` sarcinile eșuate sunt scrise întotdeauna, dar cele reușite
  doar când sunt lente: numărul de înregistrări nu este atunci numărul de
  sarcini (raportul dă și totalul, din agregate).
- **Unexpected dialogs.** FAIL dacă a apărut o fereastră de dialog neprevăzută
  de scenariu (titlul și textul ei sunt în detalii).
- **Page loads.** FAIL dacă o pagină nu s-a încărcat (timeout sau eroare).
- **Diagnostics log completeness.** WARN dacă aplicația a trebuit să renunțe
  la înregistrări de diagnostic (cifrele din raport sunt atunci minime).

Secțiunile următoare: mediul (inventarul PC-ului, setările aplicației –
inclusiv numărul efectiv de fire pentru procesare, valoarea setată și
prioritatea lor –, paginile generate și compresia lor TIFF, ecranele, testul
de stocare cu explicații), blocajele (histogramă, în timp, tabel cu stiva de
apeluri), durata operațiilor, viteza etapelor (secunde pe pagină, pe ciclu),
încărcarea paginilor și salvările automate (pe ramuri: ce a hotărât salvarea
automată să facă), resursele în timp (grafice cu marcaje la începutul
fiecărui ciclu), I/O pe disc, comparația între instanțe și lista fișierelor.

În modul `reopen`, de la al doilea ciclu etapa Output este rulată din nou pe
pagini deja procesate: aplicația doar verifică ieșirea existentă. Aceste
rulări apar separat, pe rândul „Output (re-check of finished output)”, și nu
intră în media etapei Output, în evidențierea pe cicluri sau în comparația
dintre rapoarte.

Orientativ, pentru testul de stocare: pe un SSD local o scriere sigură de
1 MB durează 1–5 ms, crearea unui fișier mic sub 1 ms, citirea atributelor
sub 0,1 ms. Peste 20 ms la scrierea sigură indică un share de rețea sau un
disc lent. Peste 1 ms la atribute indică, pe un share, latența rețelei, iar
pe un disc local un antivirus/EDR sau alt filtru al sistemului de fișiere
(raportul spune care dintre ele se aplică, după tipul locației).

## Unde ajung fișierele

Test de stres (`%LOCALAPPDATA%\scantailor-dgi-stress\<data-ora>\`, sau
`-OutDir`). Fiecare test are nevoie de un folder nou: scriptul refuză (cod de
ieșire 2) un `-OutDir` care conține deja un test (`run.json` sau foldere
`inst1`, `inst2`, ...), pentru ca jurnalele, notele de crash și setările a două
teste să nu se amestece. Dă un folder nou sau gol, ori lasă `-OutDir` deoparte
și se creează automat un folder nou cu data și ora.

```
run.json               parametrii, timpii, codurile de ieșire, inventarul, testul de stocare
runner.log             ce a afișat scriptul
runner-samples.csv     eșantioane din exterior la 5 s (CPU, memorie, handle-uri)
operator-settings.ini  copia setărilor operatorului (doar cu -UseOperatorSettings)
scans\                 scanările copiate (doar cu -CopyScansLocal)
inst1\ ...             câte un folder pe instanță: logs\, settings\, out\, proiectul
inst1\timeout-screenshot.png   captură de ecran dacă instanța a fost oprită la timeout
report\report.html     raportul; report\summary.json pentru comparații
summary.md             verdictele pe scurt, în Markdown (apare și în sumarul jobului din CI)
```

Colectare: folderul și zip-ul `diag-<CALCULATOR>-<data-ora>` pe Desktop (sau
`-OutDir`).

## Curățenie

- Testul de stres șterge singur, la final, folderele pe care instanțele de
  test le lasă în `%LOCALAPPDATA%\scantailor-dgi\scantailor-dgi-stress-<N>`
  (după ce copiază din ele eventualele note de crash în `instN\logs\from-appdata`).
- Rezultatele testelor rămân în `%LOCALAPPDATA%\scantailor-dgi-stress\`; când
  nu mai sunt necesare, se șterge manual acest folder (pot ocupa mult spațiu,
  din cauza paginilor procesate din `out\`).
- Testul de stocare încearcă să-și șteargă subfolderul temporar
  (`st-diag-probe-xxxxxxxx`). Dacă nu reușește, calea apare ca eroare în
  consolă și în raport, iar folderul trebuie șters manual.

## Siguranță

- **Nu indica `-Scans` spre un folder de ieșire din producție.** Scanările sunt
  doar citite, dar e bine să folosești o copie sau `-CopyScansLocal`.
- Toate ieșirile testului (proiect, pagini procesate, cache) ajung numai în
  folderul testului (`-OutDir`). Scriptul refuză un `-OutDir` aflat în
  folderul cu scanări.
- Setările reale și lista de proiecte recente ale operatorului nu sunt
  atinse: instanțele de test folosesc un nume de aplicație separat
  (`scantailor-dgi-stress-<N>`), iar cu `-UseOperatorSettings` se folosește o
  copie a fișierului de setări.
- Rulează testul de stres când operatorul nu lucrează: consumă mult procesor
  și disc, iar ferestrele de test apar pe ecran.
- Jurnalele conțin căi de fișiere (nume de titluri) și numele calculatorului;
  nu conțin imagini sau conținutul documentelor.
- La timeout, scriptul face o captură a ecranului principal (pentru a vedea
  ce fereastră a blocat testul) și o salvează în folderul instanței.

## Nivelul de diagnostic al aplicației

Aplicația are trei niveluri:

| Nivel | Ce scrie |
|---|---|
| `off` | nimic |
| `basic` (implicit) | blocajele interfeței, operațiile lente (≥ 50 ms pe firul interfeței, ≥ 2 s în rest), plus toate etapele de procesare (`stage.*`, pentru fiecare pagină) și toate sarcinile eșuate; resurse la 30 s |
| `verbose` | toate operațiile, resurse la fiecare secundă (folosit automat în testul de stres) |

Nivelul se alege în două feluri (variabila de mediu are prioritate):

- variabila de mediu `SCANTAILOR_DIAG` = `off`, `basic` sau `verbose`
  (de exemplu, pentru utilizatorul curent: `setx SCANTAILOR_DIAG verbose`,
  apoi repornește Scantailor);
- în fișierul de setări al aplicației (`%APPDATA%\scantailor-dgi\scantailor-dgi.ini`
  la versiunea instalată, `<folderul aplicației>\config\scantailor-dgi\scantailor-dgi.ini`
  la cea portabilă), secțiunea:

  ```
  [diagnostics]
  level=verbose
  ```

Pentru o zi de observație detaliată pe un PC cu probleme, `verbose` este util;
nivelul `basic` poate rămâne pornit permanent. Jurnalele mai vechi de 14 zile
sunt șterse automat la pornirea aplicației.

## Parametri utili

`Run-StressTest.cmd`:

| Parametru | Implicit | Rol |
|---|---|---|
| `-Exe` | `..\scantailor.exe` lângă folderul `diagnostics` | aplicația testată |
| `-Scans` / `-Synthetic N` | pagini generate = `-Pages` | sursa paginilor |
| `-SyntheticKind gray\|rgb\|bw`, `-SyntheticDpi 300`, `-SpreadEvery N` | gray, 300, 0 | tipul paginilor generate (`-SpreadEvery 5`: fiecare a 5-a pagină e o pagină dublă) |
| `-Pages 40` | 40 | primele N imagini (0 = toate) |
| `-Instances 1` | 1 | instanțe simultane |
| `-Cycles 5` | 5 | cicluri (deschidere, procesare, navigare, salvare, închidere) |
| `-Mode reopen\|fresh` | reopen | redeschide proiectul sau începe de la zero la fiecare ciclu |
| `-NavPages 10` | 10 | câte pagini se deschid interactiv în fiecare etapă |
| `-Stages "0,1,2,3,4,5"` | toate | etapele procesate |
| `-ProbeDir "\\nas\titluri"` | – | foldere suplimentare pentru testul de stocare (mai multe: separate cu `;`) |
| `-UseOperatorSettings` | – | folosește o copie a setărilor operatorului |
| `-Settings "cheie=valoare;cheie2=valoare"` | – | setări suplimentare pentru instanțele de test |
| `-CopyScansLocal` | – | copiază întâi scanările local |
| `-TimeoutMinutes 60` | 60 | timpul maxim pentru tot testul |
| `-SkipStorageProbe`, `-QuickProbe` | – | fără / scurt test de stocare |
| `-Quick` | – | test scurt (12 pagini, 2 cicluri) |
| `-OutDir`, `-Label`, `-NoOpen` | – | unde se scrie (folder nou sau gol), eticheta din raport, fără deschiderea raportului |

Codul de ieșire al scriptului (important pentru CI): 0 = toate instanțele au
ieșit cu 0 și nu există niciun eșec „dur" (crash, cod de ieșire greșit, sarcină
eșuată, dialog neașteptat, pagină neîncărcată); 1 = altfel – o instanță care a
ieșit cu alt cod decât 0 sau a depășit timpul maxim dă întotdeauna 1, chiar
dacă și analiza a eșuat; 2 = testul nu a putut fi rulat cum s-a cerut
(parametri greșiți, un `-OutDir` care conține deja un test, o instanță care nu
a putut porni, o eroare a scriptului sau o analiză eșuată fără ca vreo
instanță să fi eșuat). Verdictele de timp și de memorie apar în raport, dar nu
schimbă codul de ieșire: pe o mașină încărcată sau partajată ele pot depăși
pragurile și fără o problemă în aplicație. `Analyze-Diagnostics.ps1` folosește
aceeași regulă (și întoarce 1 dacă nu găsește niciun jurnal).

Căile relative (`-OutDir`, `-Out`, `-ProbeDir`) sunt luate față de folderul
curent din PowerShell, la toate scripturile.

`Collect-Diagnostics.cmd`: `-AppDir`, `-Days 7`, `-IncludeDumps`, `-ProbeDir`,
`-OutDir`, `-Label`, `-QuickProbe`, `-NoAnalyze`, `-NoOpen`.

`Analyze-Diagnostics.cmd -Path <folder | zip | fișier .jsonl> [-Out folder] [-Open]`.
Analiza citește jurnalul în flux, deci merge și pentru fișiere foarte mari
(un milion de linii: în jur de un minut în Windows PowerShell 5.1); pentru
operațiile cu foarte multe înregistrări, percentilele se calculează pe un
eșantion aleator uniform de 50 000 de valori (numărul, totalul și maximul
rămân exacte), iar raportul menționează acest lucru.

## Probleme frecvente

- **Fereastra se închide imediat la dublu-clic.** Pornește `.cmd`-ul dintr-o
  fereastră de comandă (`cmd`) ca să vezi mesajul, sau verifică
  `runner.log` în folderul testului.
- **„scantailor.exe not found".** Pune folderul `diagnostics` lângă aplicație
  sau dă calea: `-Exe "C:\Program Files\Scantailor-DGI\scantailor.exe"`.
- **„No log files found" la colectare.** Diagnosticul poate fi oprit
  (`SCANTAILOR_DIAG=off` sau `level=off`), aplicația nu a rulat în perioada
  aleasă sau este portabilă în alt folder (`-AppDir`).
- **Timeout.** Mărește `-TimeoutMinutes`; captura de ecran din folderul
  instanței arată unde s-a oprit.
- **Excepțiile Defender apar ca necitibile.** Normal fără drepturi de
  administrator; restul inventarului este complet.
