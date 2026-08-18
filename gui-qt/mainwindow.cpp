#include "mainwindow.h"
#include "command_palette.h"
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFrame>
#include <QToolBar>
#include <QStatusBar>
#include <QAction>
#include <QMenuBar>
#include <QApplication>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QCoreApplication>
#include <QShortcut>
#include <QKeySequence>
#include <QCloseEvent>
#include <QDir>
#include <QDialog>
#include <QPushButton>

MainWindow::MainWindow()
{
    drv = new DriverClient(this);

    setWindowTitle("The Goodmans Kernel");
    resize(1500, 940);
    setMinimumSize(1100, 720);

    // menu bar
    auto* mb = menuBar();
    auto* fileMenu = mb->addMenu("&File");
    auto* actLoad = fileMenu->addAction("&Load module…");
    actLoad->setShortcut(QKeySequence("Ctrl+O"));
    connect(actLoad, &QAction::triggered, this, &MainWindow::load_wasm_from_dialog);
    recentMenu = fileMenu->addMenu("Open &Recent");
    fileMenu->addSeparator();
    auto* actPal = fileMenu->addAction("Command &Palette…");
    actPal->setShortcut(QKeySequence("Ctrl+K"));
    connect(actPal, &QAction::triggered, this, &MainWindow::open_command_palette);
    fileMenu->addSeparator();
    auto* actQuit = fileMenu->addAction("E&xit");
    actQuit->setShortcut(QKeySequence("Ctrl+Q"));
    connect(actQuit, &QAction::triggered, qApp, &QApplication::quit);

    auto* viewMenu = mb->addMenu("&View");
    QStringList page_labels = { "Modules", "Explorer", "Trace", "Profiler", "Events", "Log", "Memory", "Deploy" };
    for (int i = 0; i < page_labels.size(); i++) {
        auto* a = viewMenu->addAction(QString("&%1 %2").arg(i + 1).arg(page_labels[i]));
        a->setShortcut(QKeySequence(QString("Ctrl+%1").arg(i + 1)));
        connect(a, &QAction::triggered, this, [this, i]{ go_to_page(i); });
    }
    viewMenu->addSeparator();
    auto* actRefresh = viewMenu->addAction("&Refresh");
    actRefresh->setShortcut(QKeySequence("F5"));
    connect(actRefresh, &QAction::triggered, this, &MainWindow::tick_slow);

    auto* driverMenu = mb->addMenu("&Driver");
    auto* actStart = driverMenu->addAction("Start service");
    connect(actStart, &QAction::triggered, this, [this]{ drv->run_sc("start Goodmans"); tick_slow(); });
    auto* actStop  = driverMenu->addAction("Stop service");
    connect(actStop, &QAction::triggered, this, [this]{ drv->run_sc("stop Goodmans"); tick_slow(); });
    auto* actRestart = driverMenu->addAction("Restart service");
    connect(actRestart, &QAction::triggered, this, [this]{
        drv->run_sc("stop Goodmans"); drv->run_sc("start Goodmans"); tick_slow();
    });

    auto* toolsMenu = mb->addMenu("&Tools");
    auto* actPalT = toolsMenu->addAction("Command palette");
    actPalT->setShortcut(QKeySequence("Ctrl+K"));
    connect(actPalT, &QAction::triggered, this, &MainWindow::open_command_palette);

    auto* helpMenu = mb->addMenu("&Help");
    auto* actAbout = helpMenu->addAction("&About The Goodmans Kernel");
    connect(actAbout, &QAction::triggered, this, &MainWindow::open_about);

    // toolbar (x64dbg / IDA style row of textual actions)
    auto* tbar = addToolBar("Main");
    tbar->setObjectName("mainToolbar");
    tbar->setMovable(false);
    tbar->setFloatable(false);
    tbar->addAction(actLoad);
    auto* actUnloadAll = tbar->addAction("Unload all");
    connect(actUnloadAll, &QAction::triggered, this, [this]{ drv->unload_all(); tick_slow(); });
    tbar->addSeparator();
    tbar->addAction(actStart);
    tbar->addAction(actStop);
    tbar->addAction(actRestart);
    tbar->addSeparator();
    tbar->addAction(actRefresh);
    tbar->addSeparator();
    tbar->addAction(actPalT);

    // tab bar as main navigation
    tabs = new QTabWidget;
    tabs->setObjectName("mainTabs");
    tabs->setDocumentMode(true);
    tabs->setUsesScrollButtons(false);
    setCentralWidget(tabs);

    workbench = new WorkbenchPage(drv);
    explorerp = new ExplorerPage(drv);
    tracep    = new TracePage(drv);
    profilerp = new ProfilerPage;
    events    = new EventsPage(drv);
    connect(events, &EventsPage::toast, this, &MainWindow::show_toast);
    logp      = new LogPage;
    memory    = new MemoryPage(drv);
    deploy    = new DeployPage(drv);
    tabs->addTab(workbench, "Modules");
    tabs->addTab(explorerp, "Explorer");
    tabs->addTab(tracep,    "Trace");
    tabs->addTab(profilerp, "Profiler");
    tabs->addTab(events,    "Events");
    tabs->addTab(logp,      "Log");
    tabs->addTab(memory,    "Memory");
    tabs->addTab(deploy,    "Deploy");
    tabs->setCurrentIndex(0);

    // status bar (permanent widgets on the right, temporary toasts on the left)
    statDevice = new QLabel;
    statSvc    = new QLabel;
    statCount  = new QLabel;
    for (auto* l : { statDevice, statSvc, statCount }) l->setStyleSheet("font-size:11.5px;");
    set_stat(statDevice, "driver ?",  "#808080", "#666666");
    set_stat(statSvc,    "service ?", "#808080", "#666666");
    set_stat(statCount,  "0 modules", "#808080", "");
    auto* sep1 = new QLabel(" | "); sep1->setStyleSheet("color:#444444;");
    auto* sep2 = new QLabel(" | "); sep2->setStyleSheet("color:#444444;");
    statusBar()->addPermanentWidget(statDevice);
    statusBar()->addPermanentWidget(sep1);
    statusBar()->addPermanentWidget(statSvc);
    statusBar()->addPermanentWidget(sep2);
    statusBar()->addPermanentWidget(statCount);

    // wiring
    connect(tabs, &QTabWidget::currentChanged, this, &MainWindow::on_nav_changed);
    connect(workbench, &WorkbenchPage::moduleSelected, this, &MainWindow::on_module_selected);
    connect(workbench, &WorkbenchPage::moduleLoaded,   this, &MainWindow::on_module_loaded);
    connect(workbench, &WorkbenchPage::toast,          this, &MainWindow::show_toast);
    connect(deploy,    &DeployPage::stateChanged,     this, &MainWindow::on_state_changed);
    connect(deploy,    &DeployPage::toast,            this, &MainWindow::show_toast);
    connect(memory,    &MemoryPage::toast,            this, &MainWindow::show_toast);
    connect(tracep,    &TracePage::toast,             this, &MainWindow::show_toast);
    connect(explorerp, &ExplorerPage::toast,          this, &MainWindow::show_toast);

    connect(&fast_timer, &QTimer::timeout, this, &MainWindow::tick_fast);
    connect(&slow_timer, &QTimer::timeout, this, &MainWindow::tick_slow);
    fast_timer.start(300);
    slow_timer.start(2000);

    // hot reload watcher (guest .wasm and driver .sys)
    hot_watcher = new QFileSystemWatcher(this);
    connect(hot_watcher, &QFileSystemWatcher::fileChanged, this, &MainWindow::on_file_changed);

    // auto-watch the driver binary too; on change: stop/start service
    QString here_dir = QCoreApplication::applicationDirPath();
    QStringList driver_candidates = {
        here_dir + "/../Goodmans.sys",   // same layout as deploy/
        here_dir + "/../../Goodmans.sys",
    };
    for (const auto& p : driver_candidates) {
        QString abs = QFileInfo(p).absoluteFilePath();
        if (QFile::exists(abs)) { hot_watcher->addPath(abs); break; }
    }

    // global shortcuts
    auto* palSc = new QShortcut(QKeySequence("Ctrl+K"), this);
    connect(palSc, &QShortcut::activated, this, &MainWindow::open_command_palette);
    auto* findSc = new QShortcut(QKeySequence("Ctrl+F"), this);
    connect(findSc, &QShortcut::activated, this, [this]{
        if (tabs->currentIndex() == 2) tracep->focusFilter();
    });

    load_settings();
    rebuild_recent_menu();
    tick_slow();

    // always land on Modules on launch, regardless of saved page
    tabs->setCurrentIndex(0);
}

void MainWindow::closeEvent(QCloseEvent* e)
{
    save_settings();
    QMainWindow::closeEvent(e);
}

void MainWindow::set_stat(QLabel* lbl, const QString& txt, const QString& color, const QString& dot_color)
{
    if (!dot_color.isEmpty()) {
        lbl->setText(QString("<span style='color:%1;'>●</span> <span style='color:%2;'>%3</span>")
                     .arg(dot_color, color, txt.toHtmlEscaped()));
    } else {
        lbl->setText(QString("<span style='color:%1;'>%2</span>").arg(color, txt.toHtmlEscaped()));
    }
}

void MainWindow::on_nav_changed(int row)
{
    if (row >= 0) tabs->setCurrentIndex(row);
}

void MainWindow::go_to_page(int i)
{
    if (i >= 0 && i < tabs->count()) tabs->setCurrentIndex(i);
}

void MainWindow::on_module_selected(quint32 id)
{
    ModuleInfo info; drv->module_info(id, info);
    workbench->setSelected(id, info);
}

void MainWindow::on_module_loaded(quint32 id)
{
    QVector<ModuleRow> mods; drv->refresh_modules(mods);
    workbench->refreshModules(mods);
    ModuleInfo info; drv->module_info(id, info);
    workbench->setSelected(id, info);
    tabs->setCurrentIndex(0);

    for (const auto& m : mods) {
        if (m.id == id) {
            add_recent(m.name);
            if (QFile::exists(m.name)) {
                path_to_module.insert(m.name, id);
                hot_watcher->addPath(m.name);
            }
            break;
        }
    }
}

void MainWindow::on_state_changed() { tick_slow(); }

void MainWindow::show_toast(const QString& msg, int kind)
{
    statusBar()->showMessage(msg, 4000);
    statusBar()->setStyleSheet(
        QString("QStatusBar { color: %1; font-size:12px; padding-left:8px; }")
        .arg(kind == 1 ? "#cd5c5c" : (kind == 2 ? "#8fbf5c" : "#d4d4d4")));
}

void MainWindow::tick_fast()
{
    QVector<LogEntry> entries; quint32 dropped = 0;
    if (drv->tail_log(log_seq, entries, dropped) && !entries.isEmpty()) {
        logp->append(entries, log_seq, dropped);
        events->append(entries);
    }
    QVector<TraceEvent> tr; quint32 tdrop = 0;
    if (drv->tail_trace(trace_seq, tr, tdrop) && !tr.isEmpty()) {
        tracep->append(tr);
        profilerp->observe(tr);
    }
}

void MainWindow::tick_slow()
{
    bool dev = drv->device_open();
    bool pres = drv->service_present();
    bool run  = drv->service_running();
    set_stat(statDevice, dev ? "driver loaded" : "driver not loaded",
             "#cccccc", dev ? "#8fbf5c" : "#cd5c5c");
    set_stat(statSvc,    run ? "service running" : (pres ? "service stopped" : "service not installed"),
             "#cccccc", run ? "#8fbf5c" : (pres ? "#e5b055" : "#cd5c5c"));
    QVector<ModuleRow> mods; drv->refresh_modules(mods);
    workbench->refreshModules(mods);
    int user_count = 0;
    for (const auto& m : mods)
        if (QFileInfo(m.name).fileName().compare("toolkit.wasm", Qt::CaseInsensitive) != 0)
            user_count++;
    set_stat(statCount, QString("%1 modules").arg(user_count), "#999999", "");
    deploy->refresh(dev, pres, run);
}

void MainWindow::load_wasm_from_dialog()
{
    QString path = QFileDialog::getOpenFileName(this, "Load module", QString(), "WebAssembly (*.wasm)");
    if (path.isEmpty()) return;
    quint32 id = 0; QString err;
    if (drv->load_module(path, 0, id, err)) {
        show_toast(QString("loaded module %1").arg(id), 2);
        on_module_loaded(id);
    } else {
        show_toast("load failed: " + err, 1);
    }
}

void MainWindow::reload_current_selection()
{
    // reload every tracked wasm from disk
    for (auto it = path_to_module.begin(); it != path_to_module.end(); ++it) {
        drv->unload(it.value());
    }
    QHash<QString, quint32> newmap;
    for (auto it = path_to_module.begin(); it != path_to_module.end(); ++it) {
        quint32 id = 0; QString err;
        if (drv->load_module(it.key(), 0, id, err)) {
            newmap.insert(it.key(), id);
        }
    }
    path_to_module = newmap;
    tick_slow();
    show_toast(QString("reloaded %1 modules").arg(newmap.size()), 2);
}

void MainWindow::on_file_changed(const QString& path)
{
    // some editors atomically-replace on write; watcher stops after that. re-add.
    if (QFile::exists(path)) hot_watcher->addPath(path);

    // driver .sys changed: restart service
    if (path.endsWith("Goodmans.sys", Qt::CaseInsensitive)) {
        show_toast("driver changed - restarting service", 0);
        drv->run_sc("stop Goodmans");
        drv->run_sc("start Goodmans");
        // reload every previously-loaded wasm since the driver forgot them
        QHash<QString, quint32> old = path_to_module;
        path_to_module.clear();
        for (auto it = old.begin(); it != old.end(); ++it) {
            quint32 id = 0; QString err;
            if (drv->load_module(it.key(), 0, id, err)) {
                path_to_module.insert(it.key(), id);
                if (it.key().endsWith("toolkit.wasm", Qt::CaseInsensitive))
                    explorerp->setToolkitId((int)id);
            }
        }
        tick_slow();
        show_toast(QString("driver restarted, %1 modules reloaded").arg(path_to_module.size()), 2);
        return;
    }

    if (!path_to_module.contains(path)) return;
    quint32 old_id = path_to_module.value(path);
    drv->unload(old_id);
    quint32 new_id = 0; QString err;
    if (drv->load_module(path, 0, new_id, err)) {
        path_to_module.insert(path, new_id);
        if (path.endsWith("toolkit.wasm", Qt::CaseInsensitive))
            explorerp->setToolkitId((int)new_id);
        show_toast(QString("hot reload: %1: module %2").arg(QFileInfo(path).fileName()).arg(new_id), 2);
        tick_slow();
    } else {
        show_toast("hot reload failed: " + err, 1);
    }
}

void MainWindow::add_recent(const QString& path)
{
    recent_files.removeAll(path);
    recent_files.prepend(path);
    while (recent_files.size() > 10) recent_files.removeLast();
    rebuild_recent_menu();
}

void MainWindow::rebuild_recent_menu()
{
    if (!recentMenu) return;
    recentMenu->clear();
    if (recent_files.isEmpty()) {
        auto* a = recentMenu->addAction("(no recent files)");
        a->setEnabled(false);
        return;
    }
    for (const auto& p : recent_files) {
        QString label = QFileInfo(p).fileName();
        auto* a = recentMenu->addAction(label);
        a->setToolTip(p);
        connect(a, &QAction::triggered, this, [this, p]{
            if (!QFile::exists(p)) { show_toast("file no longer exists", 1); return; }
            quint32 id = 0; QString err;
            if (drv->load_module(p, 0, id, err)) { show_toast(QString("loaded %1").arg(id), 2); on_module_loaded(id); }
            else show_toast(err, 1);
        });
    }
    recentMenu->addSeparator();
    auto* clr = recentMenu->addAction("Clear recent");
    connect(clr, &QAction::triggered, this, [this]{ recent_files.clear(); rebuild_recent_menu(); });
}

void MainWindow::save_settings()
{
    QSettings s("goodmans", "gui");
    s.setValue("geometry", saveGeometry());
    s.setValue("state",    saveState());
    s.setValue("page",     tabs->currentIndex());
    s.setValue("recent",   recent_files);
}

void MainWindow::load_settings()
{
    QSettings s("goodmans", "gui");
    if (s.contains("geometry")) restoreGeometry(s.value("geometry").toByteArray());
    if (s.contains("state"))    restoreState(s.value("state").toByteArray());
    recent_files = s.value("recent").toStringList();
}

void MainWindow::open_about()
{
    QDialog dlg(this);
    dlg.setWindowTitle("About The Goodmans Kernel");
    dlg.setFixedSize(560, 520);
    dlg.setStyleSheet("QDialog { background: #1c1c1c; }");

    auto* v = new QVBoxLayout(&dlg);
    v->setContentsMargins(28, 24, 28, 20);
    v->setSpacing(4);

    auto* title = new QLabel("The Goodmans Kernel");
    title->setStyleSheet("color:#ffffff; font-size:22px; font-weight:600;");
    v->addWidget(title);

    auto* tag = new QLabel("Signed WDM driver embedding wasm3.");
    tag->setStyleSheet("color:#8b8b8b; font-size:12.5px;");
    v->addWidget(tag);

    auto* sep = new QFrame; sep->setFrameShape(QFrame::HLine);
    sep->setStyleSheet("background:#2c2c2c; max-height:1px; border:none; margin-top:14px; margin-bottom:14px;");
    v->addWidget(sep);

    auto* project = new QLabel(
        "<p style='color:#dcdcdc; font-size:12.5px; line-height:1.55;'>"
        "Loads unsigned <b>.wasm</b> modules into a signed kernel driver."
        "</p>"
    );
    project->setWordWrap(true);
    project->setTextFormat(Qt::RichText);
    v->addWidget(project);

    v->addSpacing(6);

    auto* authorLbl = new QLabel("Author");
    authorLbl->setStyleSheet("color:#8b8b8b; font-size:11px; font-weight:600;");
    v->addWidget(authorLbl);
    auto* author = new QLabel(
        "<p style='color:#dcdcdc; font-size:12.5px; line-height:1.55;'>"
        "<b>zer0condition</b>"
        "</p>"
    );
    author->setWordWrap(true);
    author->setTextFormat(Qt::RichText);
    v->addWidget(author);

    v->addSpacing(6);

    auto* stackLbl = new QLabel("Built with");
    stackLbl->setStyleSheet("color:#8b8b8b; font-size:11px; font-weight:600;");
    v->addWidget(stackLbl);
    auto* stack = new QLabel(
        "<p style='color:#c8c8c8; font-size:12px; line-height:1.55;'>"
        "&bull; wasm3 , MIT, Volodymyr Shymanskyy / Steven Massey<br>"
        "&bull; Qt 6 , LGPLv3, The Qt Company<br>"
        "&bull; JetBrainsMono , OFL, JetBrains"
        "</p>"
    );
    stack->setTextFormat(Qt::RichText);
    v->addWidget(stack);

    v->addStretch();

    auto* btnRow = new QHBoxLayout;
    btnRow->addStretch();
    auto* close = new QPushButton("Close");
    close->setObjectName("btnPrimary");
    close->setDefault(true);
    close->setFixedWidth(96);
    btnRow->addWidget(close);
    v->addLayout(btnRow);

    connect(close, &QPushButton::clicked, &dlg, &QDialog::accept);
    dlg.exec();
}

void MainWindow::open_command_palette()
{
    QVector<CommandAction> actions;
    // navigation
    QStringList pages = { "Modules", "Explorer", "Trace", "Profiler", "Events", "Log", "Memory", "Deploy" };
    for (int i = 0; i < pages.size(); i++)
        actions.push_back({ "Go: " + pages[i], QString("Ctrl+%1").arg(i + 1), [this, i]{ go_to_page(i); }});

    actions.push_back({ "Load module…",           "Ctrl+O", [this]{ load_wasm_from_dialog(); }});
    actions.push_back({ "Reload all modules",     "",       [this]{ reload_current_selection(); }});
    actions.push_back({ "Unload all modules",     "",       [this]{ drv->unload_all(); tick_slow(); show_toast("unloaded all", 2); }});
    actions.push_back({ "Refresh state",          "F5",     [this]{ tick_slow(); }});

    actions.push_back({ "Trace: Start / Stop",    "F5 in trace", [this]{ go_to_page(2) /* trace */; tracep->toggleTracing(); }});
    actions.push_back({ "Trace: Focus filter",    "Ctrl+F", [this]{ go_to_page(2) /* trace */; tracep->focusFilter(); }});
    actions.push_back({ "Trace: Save…",           "Ctrl+S", [this]{
        go_to_page(2) /* trace */;
        QString p = QFileDialog::getSaveFileName(this, "Save trace", "trace.gtrace",
            "Goodmans trace (*.gtrace);;CSV (*.csv)");
        if (!p.isEmpty()) tracep->saveTraceFile(p);
    }});
    actions.push_back({ "Trace: Open .gtrace…",   "",       [this]{
        go_to_page(2) /* trace */;
        QString p = QFileDialog::getOpenFileName(this, "Open trace", QString(), "Goodmans trace (*.gtrace)");
        if (!p.isEmpty()) tracep->loadTraceFile(p);
    }});
    actions.push_back({ "Trace: Toggle bookmark", "Ctrl+B", [this]{ go_to_page(2) /* trace */; tracep->toggleBookmark(); }});

    actions.push_back({ "Driver: Start service",  "",       [this]{ drv->run_sc("start Goodmans"); tick_slow(); }});
    actions.push_back({ "Driver: Stop service",   "",       [this]{ drv->run_sc("stop Goodmans"); tick_slow(); }});
    actions.push_back({ "Help: About",            "",       [this]{ open_about(); }});

    for (const auto& p : recent_files) {
        QString label = "Open recent: " + QFileInfo(p).fileName();
        actions.push_back({ label, "", [this, p]{
            if (!QFile::exists(p)) { show_toast("file gone", 1); return; }
            quint32 id = 0; QString err;
            if (drv->load_module(p, 0, id, err)) { on_module_loaded(id); show_toast(QString("loaded %1").arg(id), 2); }
            else show_toast(err, 1);
        }});
    }

    CommandPalette p(this, actions);
    QRect g = geometry();
    p.move(g.center().x() - p.width() / 2, g.top() + 80);
    p.exec();
}
