# PulseWatch — Signal Quiet

Telemetria nativa do Windows: AppBar no topo com rede, CPU, RAM, GPU e ventoinha.

## Identidade

- **Marca:** glyph ECG em Signal Teal (`#2F9E8A`) — sem wordmark na barra
- **Cor = estado:** neutro no normal; amber em warning; coral em crítico
- **Tipo:** Cascadia Mono / Consolas nas métricas (figuras tabulares)

## Presets de densidade

| Preset | Comportamento |
|--------|----------------|
| Quiet | Só valores · 32px |
| Focus | Micro-sparklines inline · 40px |
| Gaming | Prioriza GPU/FAN · esconde rede se tráfego estável · oculta CPU |

## Interação

- **Clique esquerdo** em uma métrica → histórico sparkline de 60s
- **Clique direito** na barra ou bandeja → preferências
- **Clique esquerdo** no ícone da bandeja → Sobre
- **Duplo clique** na bandeja → encerra
- **Ctrl+Alt+Shift+P** → atalho de emergência para encerrar

## Build

```bat
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cd native
cmake -S . -B build -G "Ninja" 2>nul || cmake -S . -B build -G "NMake Makefiles"
cmake --build build
```

Ou compile direto:

```bat
cl /EHsc /std:c++17 /DUNICODE /D_UNICODE /O2 src\main.cpp /Fe:PulseWatch.exe /link shell32.lib iphlpapi.lib user32.lib gdi32.lib advapi32.lib d2d1.lib dwrite.lib
```

## Encerrar

`native\Encerrar-PulseWatch.cmd` ou o atalho de emergência acima.
