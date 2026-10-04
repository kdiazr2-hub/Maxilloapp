---
type: plan
title: Plan — Guía de corte Le Fort I generada a partir del movimiento
description: Plan técnico (contratos, flujos, tests) de la guía Le Fort I que se deriva de los cambios de plano del segmento y propone sus orificios en hueso con soporte.
tags: [sdd, plan, guias, lefort]
timestamp: 2026-10-04T00:00:00Z
topic: sdd
slug: guia-lefort-por-movimiento
status: approved
---

# Plan — Guía de corte Le Fort I generada a partir del movimiento

> Spec: [../specs/guia-lefort-por-movimiento.md](../specs/guia-lefort-por-movimiento.md) ·
> Constitution: [../constitution.md](../constitution.md) v1.0.0 · Status: approved (usuario, 2026-10-04)
> Last updated: 2026-10-04

## 0. Global Constraints

- **Stack:** C++17 · Qt 6.11 · VTK 9.5.2 · MSVC; todo target nuevo con `/utf-8` (constitución 1, 3).
- **Lógica nueva fuera de `MainWindow`:** en clases core sin Qt Widgets, cada una con un test
  `add_core_test()` (constitución 5). `MainWindowGuides.cpp` solo orquesta.
- **Medir contra el hueso, nunca contra un wrap:** el grosor y el asiento de un orificio se miden en
  `ImplicitCore::BakeMeshField` de las mallas óseas preoperatorias (constitución 8, `CLAUDE.md`
  HOW TO MEASURE).
- **Marco del paciente:** orientado (Frankfurt); vertical = +Z, anterior = −Y (DICOM LPS), como en
  `guideMotionSummary` y REPOSICIÓN. En los tests sintéticos de `LeFortGuideTests` la cara mira a +Y.
- **Valores de la spec, exactos:** grosor óseo mínimo bajo un tornillo **2.0 mm**; umbral de franja
  **0.5 mm**; tolerancia de altura **±0.5 mm**; orificios por defecto **2 encima + 2 debajo del corte
  en el reborde piriforme y en el pilar cigomático de cada lado (16 en total)**; **dos ranuras** en
  impactación (corte Le Fort + borde superior de la franja); solo cuentan los **cambios de plano**
  (vertical, rotación horaria/antihoraria, inclinación lateral); avance, desplazamiento lateral y
  giro horizontal **no** cambian la guía.
- **Reglas que se mantienen:** un orificio en borde óseo libre o a < 4 mm del corte se **rechaza**
  (`PlateCore::CheckHoleSeat`, regla del usuario 2026-09-20); soporte < 2.0 mm se **avisa y se
  respeta**. La guía sale de **una pieza** y toda camisa tiene material debajo.
- **Proyectos:** toda clave nueva del plan es opcional; un `.maxilloproject` antiguo abre igual
  (constitución 9).
- **Texto de UI e informes:** en español (constitución 7). Colores nuevos desde `CranioPalette`
  (constitución 16).
- **Commits:** gitmoji + frase en inglés; rama + PR; autor humano con `Co-Authored-By` de la IA
  permitido (constitución 10-12). Sin datos de pacientes en el repo: tests con geometría sintética
  (constitución 14).

## 1. Context & constraints

- Criterios que fijan el diseño: spec §Acceptance 1-6 (altura punto a punto, asimetría, rotación,
  avance ignorado, descenso) → el cálculo de la franja es una función pura del movimiento y del
  corte. §Acceptance 7-9 (orificios, edición, aviso) → orificios como datos del plan, editables y
  re-evaluables. §Acceptance 10 (reabrir) → persistencia opcional. §Acceptance 11 (sin
  REPOSICIÓN). §Acceptance 12-13 (una pieza, revisión antes de exportar).
- Lo que ya existe y se reutiliza: `PlateCore::RigidMotion` (movimiento pre → planificado del
  segmento, vía `MainWindow::guideSegmentMotion`), `OsteotomyCore::LeFortPath` (4 puntos: pilar D,
  piriforme D, piriforme I, pilar I), `PlateCore::CheckHoleSeat` / `BoneNormalAt` / `SleeveFigures`,
  `LeFortGuideCore::Layout` (banda calada, puentes, conectores, tornillos de posicionamiento) y
  `GuideDesignCore::Build` (un solo campo).
- No funcional: la generación es interactiva; proponer los orificios no debe añadir más de unos
  segundos al `Layout` actual (muestreo acotado a las zonas de los pilares).
- Fuera de alcance que el diseño NO debe tocar: diseño de placas y su reproducción del avance,
  interferencias posteriores, BSSO/mentón, raíces dentarias, injerto (spec §Non-goals). El módulo
  «PLACAS A MEDIDA» no se modifica en este ciclo.

## 2. Architecture

```text
 REPOSICIÓN ──(mallas pre/post del segmento)──▶ PlateCore::RigidMotion ──▶ motion (4×4)
                                                                              │
 Osteotomía ──(corte Le Fort preop, 4 puntos)─────────────────────────────────┤
                                                                              ▼
                                                    [ LeFortMotionCore ]  (NUEVO, puro)
                                                    BandProfile: alturas por punto,
                                                    tramos con franja, corte superior,
                                                    clasificación
                                                          │            │
     hueso preop (BakedField) ──▶ [ LeFortHoleCore ] ◀────┘            │
     envolvente preop ─────────▶  (NUEVO) propone y evalúa             │
                                  orificios (grosor, asiento)          │
                                          │ ProposedHole[]             │
                                          ▼                            ▼
                                 [ LeFortGuideCore::Layout ]  (EXTENDIDO)
                                 banda calada que cubre los dos cortes,
                                 ranura inferior + ranura superior con puentes,
                                 almohadillas/camisas en los orificios
                                          │ LeFortGuideLayout
                                          ▼
                                 [ GuideDesignCore::Build ] (sin cambios) ──▶ malla de la guía
                                          ▲
 [ GuidePlanCore ] (EXTENDIDO) guarda los orificios (auto/manual) ── .maxilloproject
                                          ▲
 [ MainWindowGuides ] (orquesta) «Generar guía de corte», franja roja en 3D, informe,
                      modo «Mover orificio», aviso/rechazo
```

- **LeFortMotionCore** (interno, nuevo) — convierte el movimiento rígido del segmento y el corte
  preoperatorio en el perfil de la franja. Sin mallas, sin Qt Widgets: matemática pura.
- **LeFortHoleCore** (interno, nuevo) — propone los 16 orificios y evalúa cualquier sitio (grosor
  óseo a lo largo del eje de broca + reglas de asiento existentes).
- **LeFortGuideCore** (interno, extendido) — dispone la guía a partir del perfil de franja y de los
  orificios en lugar de las placas.
- **GuidePlanCore** (interno, extendido) — persiste los orificios del Le Fort, con su origen
  (automático/manual).
- **MainWindowGuides** (UI) — orquesta, muestra y permite editar. Ninguna regla nueva vive aquí.

**Decisión principal: la altura de la franja es la componente vertical del desplazamiento rígido
completo en cada punto del corte**, `h(p) = Z · (M·p − p)`, en lugar de descomponer el movimiento en
ángulos de Euler y filtrar el avance. Motivos: (1) es exacto y sin convenciones de ejes: la traslación
anteroposterior y lateral no tiene componente Z, y el giro horizontal (yaw) no mueve la tercera fila
de la rotación cuando se separa como `Rz·R_inclinación`, así que la fórmula ya cumple "solo cambios
de plano" (spec, decisión del usuario 2026-10-04); (2) `h` es afín en la posición, así que entre dos
puntos del corte es lineal y basta interpolar los 4 puntos del Le Fort (más los cruces por el umbral)
para trazar exactamente el corte superior; (3) es la misma medida que REPOSICIÓN ya muestra al
usuario («Z +x impactación»), así que lo que ve y lo que imprime coinciden. Alternativa descartada:
descomponer en Euler y reconstruir un movimiento filtrado — depende del orden de ejes y del pivote, y
da alturas distintas para el mismo plan según la convención.

## 3. Interfaces & contracts

```text
LeFortMotionCore.Band(cut: OsteotomyPath (preop, 4 puntos),
                      motion: Matrix4 (pre → planificado),
                      vertical: Vec3 = +Z,
                      thresholdMm: 0.5) -> BandProfile | InvalidPath | NoMotion
  BandProfile:
    heights[i]          altura Z·(M·p_i − p_i) en cada punto del corte (+ sube, − baja), mm
    spans[]             tramos [inicio, fin] a lo largo del corte donde altura ≥ threshold,
                        con los extremos interpolados en el cruce exacto del umbral
    upperCut            OsteotomyPath: cada punto desplazado +altura·vertical (solo con sentido
                        dentro de spans); mismos ejes de barrido que el corte
    kind                Impactación | Descenso | Mixto | SinCambioDePlano
    pitch               Antihoraria (sube más adelante) | Horaria | Ninguna   (pilares vs piriformes)
    cant                Derecha más alta | Izquierda más alta | Ninguna       (lado D vs I)
    report              texto en español con la altura en los 4 puntos y la máxima
  - NoMotion cuando |traslación| < 0.2 mm y giro < 0.3° (mismo umbral que guideMotionSummary).
  - Invariante: un movimiento sin componente Z en ningún punto (avance puro, lateral, yaw) da
    spans vacíos y kind = SinCambioDePlano.
  - Invariante: heights no cambia si a motion se le compone una traslación horizontal.

LeFortHoleCore.Support(site: Vec3, axis: Vec3,
                       bone: BakedField (mallas óseas preop),
                       seat: (consulta de asiento de PlateCore::CheckHoleSeat),
                       band: BandProfile, params) -> HoleSupport
  HoleSupport: thicknessMm, verdict ∈ {Ok, Aviso, Rechazo}, reason (español)
  - thicknessMm = recorrido a lo largo de −axis desde la superficie hasta salir del material
    (campo > 0), con paso ≤ ¼ del espaciado del campo y tope de 15 mm.
  - Rechazo: lo que hoy rechaza CheckHoleSeat (borde libre, < 4 mm del corte, lado equivocado) y,
    NUEVO, un orificio craneal dentro de la franja o a < 4 mm de su borde superior (ese hueso se quita).
  - Aviso: thicknessMm < 2.0.
  - Ok: el resto.

LeFortHoleCore.Propose(surface: envolvente preop (malla) + bone: BakedField,
                       cut: OsteotomyPath, band: BandProfile, params) -> ProposedHoles
  ProposedHoles: holes[] (ProposedHole), missing[] (pilar, lado del corte, motivo)
  ProposedHole: center, axis (= PlateCore::BoneNormalAt), pillar ∈ {PiriformeD, PilarD, PiriformeI,
                PilarI}, side ∈ {Craneal, Segmento}, origin = Automatico, support: HoleSupport
  - Por pilar y por lado del corte: los 2 sitios Ok de mayor grosor, separados ≥ 6.5 mm entre sí
    (sin solapar anillos de 5.6 mm), en la ventana vertical [4, 12] mm desde el corte (lado
    segmento) o desde el borde superior de la franja (lado craneal), y lateralmente a ±8 mm del
    punto del pilar en el corte.
  - Si no hay 2 sitios Ok, propone los que haya y lo anota en missing[] (spec §Behaviour "sin sitio
    válido"). Nunca propone un sitio con veredicto distinto de Ok.
  - Determinista: mismas entradas → mismos orificios.

LeFortGuideCore.Layout(preop, wrapMesh, cut, holes: ProposedHole[] (como PredictiveHole: preopCenter,
                       preopAxis), band: BandProfile = vacío, params) -> LeFortGuideLayout
  - Con band vacío se comporta exactamente como hoy (tests actuales intactos).
  - Con spans: añade piezas de ranura sobre band.upperCut limitadas a spans, con los mismos puentes
    (siempre uno en la línea media); la banda pintada cubre desde el corte hasta el corte superior
    + margen; las celdas del entramado quedan a ≥ latticeSlitClearMm de las dos ranuras; los
    tornillos de posicionamiento craneales quedan por encima de la franja.
  - Postcondición: layout.report incluye el texto de BandProfile.report.

GuidePlanCore (JSON del plan):
  lefortHoles: [{center, axis, pillar, side, origin: "auto" | "manual"}]   — clave opcional
  - Sin la clave, el plan carga como antes. La franja NO se guarda: se recalcula del movimiento.
```

## 4. Data model & flow

**Entidades**

- **BandProfile** — derivado, no persistido: alturas por punto, tramos, corte superior, clasificación.
- **ProposedHole / orificio del Le Fort** — persistido en el plan: posición, eje, pilar, lado,
  origen (auto/manual). El veredicto de soporte se recalcula al cargar y al generar.
- **LeFortGuideLayout** — sin cambios de forma; más piezas de ranura y más pintura cuando hay franja.

**Flujo principal** (spec: impactación anterior asimétrica)

1. El usuario pulsa «Generar guía de corte» en GUÍAS con tipo Le Fort.
2. `MainWindowGuides` obtiene el movimiento (`guideSegmentMotion`) y el corte preop
   (`guideLeFortPath`); asegura la envolvente y el campo óseo preop (`computeGuideWrap`).
3. `LeFortMotionCore.Band` → perfil. Sin movimiento: mensaje «falta el movimiento», perfil vacío,
   se sigue con una sola ranura.
4. Orificios: los `manual` guardados se conservan y se re-evalúan; los `auto` se descartan y
   `LeFortHoleCore.Propose` vuelve a proponer, sin pisar los pilares/lados que ya tienen manuales.
5. `LeFortGuideCore.Layout` con orificios + perfil → plan de la guía (pintura, ranuras, celdas,
   tornillos). Las camisas salen de `PlateCore::SleeveFigures` sobre los mismos orificios.
6. `GuideDesignCore::Build` construye la malla; se muestra la guía, la franja en rojo sobre la pared
   anterior (cinta entre corte y corte superior, solo en spans) y el informe (alturas, orificios,
   avisos, faltantes).
7. «Mover orificio»: el usuario elige un orificio y hace clic en el hueso. `Support` decide:
   Rechazo → no se mueve, motivo en la barra de estado; Aviso → se mueve, marcador en color de aviso
   y motivo en el informe; Ok → se mueve. El orificio pasa a `manual` y la guía se reconstruye.
8. Guardar → `lefortHoles` en el plan. Reabrir → se recargan; la franja se recalcula.

- Consistencia: la franja y los orificios se recalculan siempre desde el movimiento guardado; no hay
  copia que pueda quedar desfasada.
- Migración: ninguna destructiva; una clave opcional nueva. Los proyectos con placas siguen abriendo;
  en este ciclo la guía ya no toma sus orificios de las placas (spec, suposición "sustituye") y el
  informe avisa si hay placas cuyos orificios no coinciden con los de la guía.

## 5. Testing strategy

Todo en CTest con geometría sintética (`tests/SplintTestGeometry.h`, la apertura sintética de
`LeFortGuideTests`). Nada usa datos de pacientes.

| Criterio de la spec | Nivel | Afirma | Simula |
| --- | --- | --- | --- |
| §Acc 1 impactación uniforme 3 mm | unit (`LeFortMotionTests`) | heights = 3 ± 0.5 en los 4 puntos; upperCut 3 mm por encima | movimiento = traslación Z |
| §Acc 2 asimetría 4 D / 1 I | unit | heights D ≈ 4, I ≈ 1 (±0.5); cant = Derecha | rotación sobre el eje AP + traslación |
| §Acc 3 impactación anterior 4 mm | unit | piriformes > pilares; pitch = Antihoraria | rotación sobre el eje lateral |
| §Acc 4 avance puro 5 mm | unit | spans vacío; kind = SinCambioDePlano | traslación −Y |
| §Acc 5 avance 5 + impactación 3 | unit | heights iguales a §Acc 1 | traslación −Y + Z |
| §Acc 6 descenso 3 mm | unit | spans vacío; kind = Descenso | traslación −Z |
| §Behaviour mixto / umbral | unit | span acaba en el cruce interpolado; 0.4 mm → sin franja | rotación que cruza cero |
| §Acc 11 sin REPOSICIÓN | unit | NoMotion | identidad |
| §Acc 7 16 orificios con ≥ 2 mm | integración core (`LeFortHoleTests`) | 2+2 por pilar y lado, todos Ok, grosor ≥ 2.0, separados ≥ 6.5 | apertura sintética con paredes de 3 mm |
| §Behaviour sin sitio válido | integración core | pared de 1 mm en un pilar → ese pilar en missing[], ninguno propuesto allí | pared sintética delgada |
| Orificio craneal no cae en la franja | integración core | con franja de 4 mm, todos los craneales a ≥ 4 mm sobre el corte superior | franja sintética |
| §Acc 9 mover a sitio delgado | integración core | Support = Aviso con motivo; a borde libre = Rechazo | pared delgada / borde |
| §Acc 12 una pieza, camisas con material | integración core (`LeFortGuideTests`) | Build → pieces = 1; cada camisa sobre pintura; ranura superior presente solo en spans; celdas lejos de ambas ranuras | apertura sintética + franja |
| §Acc 8 regenerar tras mover | integración core | Layout con un orificio movido: camisa en el sitio nuevo, resto igual | — |
| §Acc 10 guardar/reabrir | unit (`GuidePlanTests` / `ProjectSerializerTests`) | lefortHoles ida y vuelta (auto/manual); proyecto sin clave carga | JSON en memoria |
| §Acc 11 (mensaje en la UI), §Acc 13 revisión antes de exportar | workspace (`SplintWorkspaceTests`/guías, offscreen) o verificación manual del usuario | mensaje visible; la exportación solo con guía mostrada | MainWindow offscreen |

- La línea e2e es la prueba manual del usuario con `PlateProbe` y un caso real en su PC (no entra al
  repo, constitución 14).
- Lo que debe ser real: el campo óseo (`BakeMeshField`) y `GuideDesignCore::Build`; simularlos haría
  que "una pieza" y "grosor ≥ 2 mm" no signifiquen nada.

## 6. Sequencing & dependencies

0. **Spike: compilar y correr los tests core en Linux** (Qt 6.4 + VTK 9.1 de Ubuntu, solo targets
   core) — depende de: nada — serie. Si funciona, cada tarea se verifica aquí; si no, cada tarea
   termina en "compila y pasa en el PC del usuario".
1. `LeFortMotionCore` + `LeFortMotionTests` — depende de: 0 — paralelizable con 2.
2. `LeFortHoleCore.Support` (grosor + reglas de asiento + regla de franja) + tests — depende de: 0
   (la regla de franja usa el contrato de BandProfile; se puede simular) — paralelizable con 1.
3. `LeFortHoleCore.Propose` + tests — depende de: 1, 2.
4. `LeFortGuideCore.Layout` con franja y orificios propuestos + tests — depende de: 1, 3.
5. `GuidePlanCore` `lefortHoles` + tests de ida y vuelta — depende de: 3 (forma de ProposedHole) —
   paralelizable con 4.
6. `MainWindowGuides`: nuevo «Generar guía de corte», franja roja, informe, modo «Mover orificio»
   con aviso/rechazo — depende de: 4, 5.
7. `CLAUDE.md` § Architecture + `decisions.md` (constitución 17-18) — depende de: 6.
8. Verificación en el PC del usuario: build Release + `ctest` completo + caso real con PlateProbe —
   depende de: 7.

- Paralelos: 1 ∥ 2; 4 ∥ 5.
- Orden duro: la UI (6) solo cuando los contratos core (1-5) están en verde.

## 7. Risks & open decisions

**Risks**

| Risk | Trigger | Impact | Mitigation |
| --- | --- | --- | --- |
| No se puede compilar ni probar aquí (el proyecto es Windows) | cualquier tarea | errores de compilación o tests rojos descubiertos tarde, ciclos lentos con el usuario | Spike 0 (core en Linux); tareas pequeñas; el usuario corre build + ctest al final de 6 |
| El campo óseo es demasiado grueso para medir 2 mm | espaciado del campo > 0.5 mm | grosores falsos: aceptar paredes delgadas o rechazar buenas | `Support` exige espaciado ≤ 0.25 mm en la zona de los pilares (campo local) y el test de pared de 1 mm/3 mm lo prueba |
| La segmentación pierde paredes finas del seno (perforaciones) | maxilar segmentado como cáscara perforada | grosor 0 en sitios que en la TC tienen hueso | se mide el grosor en la segmentación (decisión abierta 1); el aviso no bloquea, el cirujano decide |
| Los orificios del segmento quedan cerca de los ápices | ventana [4, 12] mm bajo el corte con raíces largas | tornillo en raíz | fuera de alcance (spec); la ventana empieza alta y el informe muestra la distancia al corte de cada orificio |
| La ranura superior divide la guía | franja alta en un tramo corto | guía en dos piezas | puentes también en la ranura superior, alineados con los inferiores; test "una pieza" con franja |
| Proyectos con placas: sus orificios ya no van a la guía | proyecto antiguo con placas | guía y placas no coinciden hasta la fase de placas | aviso explícito en el informe; la fase de placas usará `lefortHoles` |
| Cambia el movimiento después de mover orificios a mano | REPOSICIÓN editada | manuales en sitios ahora inválidos | se re-evalúan al generar: aviso/rechazo visibles (spec, área no formulable; aquí se toma la opción conservadora) |

**Decisiones cerradas** (usuario, 2026-10-04)

1. **El grosor se mide en la segmentación ósea** (que sale de la TC), con el mismo campo que el resto
   del módulo, no en HU.
2. **Los orificios manuales se conservan** si cambia el movimiento y se re-evalúan (aviso/rechazo
   visibles); no se recolocan.

**Open decisions**

- Ninguna.
