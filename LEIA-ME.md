# Spectrum Analyzer (VST3) - como compilar no GitHub

Faz isto no teu Mac, no browser.

## 1. Criar a conta e o repositorio
1. Cria uma conta gratuita em github.com.
2. Clica em "+" (canto superior direito) > "New repository".
3. Nome: SpectrumAnalyzer. Deixa "Public" (as compilacoes na nuvem sao gratuitas em repositorios publicos). Clica "Create repository".

## 2. Carregar os ficheiros
1. Descompacta este zip.
2. No repositorio, clica "uploading an existing file".
3. Arrasta para la a pasta **Source** e os ficheiros **CMakeLists.txt** e **LEIA-ME.md** (NAO precisas do build.yml neste passo).
4. Clica "Commit changes".

## 3. Criar o ficheiro de compilacao
1. No repositorio: "Add file" > "Create new file".
2. No nome do ficheiro escreve exatamente: `.github/workflows/build.yml`
   (ao escreveres as barras, o GitHub cria as pastas sozinho).
3. Abre o ficheiro **build.yml** deste zip num editor de texto (TextEdit), copia TODO o conteudo e cola na caixa grande.
4. Clica "Commit changes".

## 4. Esperar e descarregar
1. Vai ao separador **Actions**. Vai aparecer "Compilar plugin" a correr (10 a 20 minutos na primeira vez).
2. Quando ficar com visto verde, abre-o e, em baixo, em **Artifacts**, descarrega "SpectrumAnalyzer-Mac".
3. Descompacta duas vezes (o GitHub poe um zip dentro de outro) ate teres a pasta/ficheiro **Spectrum Analyzer.vst3**.

## 5. Instalar no Mac
1. Copia "Spectrum Analyzer.vst3" para `~/Library/Audio/Plug-Ins/VST3`
   (no Finder: Ir > Ir para a pasta... > cola o caminho acima).
2. Abre o Terminal e cola:
   `xattr -cr ~/Library/Audio/Plug-Ins/VST3/"Spectrum Analyzer.vst3"`
   (isto tira o bloqueio de seguranca do Mac a ficheiros descarregados).
3. Ableton: Preferencias > Plug-ins > "Use VST3 System Folders" + Rescan.
   Bitwig: Settings > Locations > Plug-ins > Rescan.

## Se der erro
No separador Actions, abre a compilacao com a cruz vermelha, clica no passo que falhou e copia as ultimas linhas do erro para o Claude.

O plugin Windows fica no artefacto "SpectrumAnalyzer-Windows".
