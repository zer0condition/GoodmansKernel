#include "pages.h"
#include <windows.h>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFormLayout>
#include <QSplitter>
#include <QHeaderView>
#include <QFileDialog>
#include <QDateTime>
#include <QMessageBox>
#include <QScrollBar>
#include <QTabWidget>
#include <QFrame>
#include <QGroupBox>
#include <QTextStream>
#include <QFile>
#include <QCoreApplication>
#include <QHash>
#include <QDataStream>
#include <QRegularExpression>
#include <QFileInfo>
#include <QShortcut>
#include <QKeySequence>

static QLabel* section_label(const QString& t)
{
    auto* l = new QLabel(t); l->setObjectName("sectionLabel"); return l;
}

static QLabel* h2(const QString& t)
{
    auto* l = new QLabel(t); l->setObjectName("h2"); return l;
}

static QFrame* card()
{
    auto* f = new QFrame; f->setObjectName("card"); f->setFrameShape(QFrame::NoFrame); return f;
}

static QLabel* pill(const QString& t, const QString& kind = "")
{
    auto* l = new QLabel(t);
    l->setObjectName("pill");
    if (!kind.isEmpty()) l->setProperty("kind", kind);
    return l;
}

// ModulesPage

// WorkbenchPage

static const char* CAP_NAMES[] = {
    "ALLOC","READ_KMEM","WRITE_KMEM","MSR_R","MSR_W","PHYSMEM","CPUID_TSC","CALLBACKS","HOSTCALL","INTROSPECT"
};

WorkbenchPage::WorkbenchPage(DriverClient* d, QWidget* p) : QWidget(p), drv(d)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 8);
    root->setSpacing(14);


    // load bar - full width, minimal
    auto* loadRow = new QHBoxLayout;
    loadRow->setSpacing(8);
    loadPath = new QLineEdit;
    loadPath->setPlaceholderText("path to .wasm  (or drag file here / Ctrl+O)");
    auto* browseBtn = new QPushButton("Browse");
    auto* loadBtn   = new QPushButton("Load"); loadBtn->setObjectName("btnPrimary");
    loadBudget = new QSpinBox;
    loadBudget->setRange(0, 1 << 30);
    loadBudget->setToolTip("pool budget in bytes, 0 = 4MB default");
    loadBudget->setFixedWidth(120);
    loadBudget->setSpecialValueText("default pool");
    loadRow->addWidget(loadPath, 1);
    loadRow->addWidget(loadBudget);
    loadRow->addWidget(browseBtn);
    loadRow->addWidget(loadBtn);
    root->addLayout(loadRow);

    connect(browseBtn, &QPushButton::clicked, this, [this]{
        QString p = QFileDialog::getOpenFileName(this, "Select .wasm", QString(), "WebAssembly (*.wasm);;All (*.*)");
        if (!p.isEmpty()) loadPath->setText(p);
    });
    connect(loadBtn, &QPushButton::clicked, this, [this]{
        if (loadPath->text().isEmpty()) { emit toast("pick a .wasm first", 1); return; }
        quint32 id = 0; QString err;
        if (drv->load_module(loadPath->text(), loadBudget->value(), id, err)) {
            pending_select = id;
            emit toast(QString("loaded module %1").arg(id), 2);
            emit moduleLoaded(id);
        } else {
            emit toast(QString("load failed: %1").arg(err), 1);
        }
    });
    connect(loadPath, &QLineEdit::returnPressed, loadBtn, &QPushButton::click);

    // horizontal split: module list left, inspector+invoke right
    auto* mainSplit = new QSplitter(Qt::Horizontal);
    mainSplit->setChildrenCollapsible(false);
    mainSplit->setObjectName("wbSplit");

    // LEFT: module list
    auto* leftCol = new QWidget;
    auto* lcv = new QVBoxLayout(leftCol);
    lcv->setContentsMargins(0, 0, 0, 0);
    lcv->setSpacing(8);
    auto* leftHead = new QHBoxLayout; leftHead->setContentsMargins(2, 0, 2, 0);
    auto* leftLbl = new QLabel("Loaded"); leftLbl->setObjectName("h2");
    moduleCount = new QLabel("0"); moduleCount->setObjectName("dim");
    auto* unloadAll = new QPushButton("Unload all");
    unloadAll->setToolTip("Unload every module");
    leftHead->addWidget(leftLbl);
    leftHead->addWidget(moduleCount);
    leftHead->addStretch();
    leftHead->addWidget(unloadAll);
    lcv->addLayout(leftHead);

    moduleList = new QListWidget;
    moduleList->setObjectName("nav");   // reuse muted nav style
    moduleList->setFocusPolicy(Qt::StrongFocus);
    moduleList->setFrameShape(QFrame::NoFrame);
    lcv->addWidget(moduleList, 1);
    mainSplit->addWidget(leftCol);

    connect(unloadAll, &QPushButton::clicked, this, [this]{
        drv->unload_all();
        clearSelection();
        emit toast("unloaded all modules", 2);
    });
    connect(moduleList, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* cur, QListWidgetItem*){
        if (!cur) return;
        quint32 id = cur->data(Qt::UserRole).toUInt();
        emit moduleSelected(id);
    });

    // RIGHT: inspector + invoke wrapper (empty state OR infoPanel)
    auto* rightCol = new QWidget;
    auto* rcv = new QVBoxLayout(rightCol);
    rcv->setContentsMargins(0, 0, 0, 0);
    rcv->setSpacing(0);

    emptyPanel = new QWidget;
    auto* el = new QVBoxLayout(emptyPanel);
    el->setContentsMargins(0, 60, 0, 0);
    auto* emptyLbl = new QLabel("Select a module from the list on the left,\nor load a .wasm above.");
    emptyLbl->setAlignment(Qt::AlignCenter);
    emptyLbl->setObjectName("emptyState");
    el->addWidget(emptyLbl);
    rcv->addWidget(emptyPanel);

    infoPanel = new QWidget;
    auto* ipl = new QVBoxLayout(infoPanel);
    ipl->setContentsMargins(0, 0, 0, 0);
    ipl->setSpacing(14);

    // module header card with unload button
    auto* headCard = card();
    auto* hl = new QHBoxLayout(headCard);
    hl->setContentsMargins(20, 14, 16, 14);
    hl->setSpacing(10);
    auto* headText = new QVBoxLayout; headText->setSpacing(2);
    modTitle = new QLabel; modTitle->setObjectName("moduleTitle");
    modMeta  = new QLabel; modMeta->setObjectName("mono"); modMeta->setStyleSheet("color:#7b818c;");
    headText->addWidget(modTitle);
    headText->addWidget(modMeta);
    hl->addLayout(headText, 1);
    auto* unloadOne = new QPushButton("Unload");
    unloadOne->setObjectName("btnDanger");
    hl->addWidget(unloadOne, 0, Qt::AlignTop);
    ipl->addWidget(headCard);
    connect(unloadOne, &QPushButton::clicked, this, [this]{
        if (!current.valid) return;
        quint32 id = current.base.id;
        drv->unload(id);
        clearSelection();
        emit toast(QString("unloaded module %1").arg(id), 2);
    });

    // caps card
    auto* capCard = card();
    auto* cl = new QVBoxLayout(capCard);
    cl->setContentsMargins(20, 14, 20, 14);
    cl->setSpacing(8);
    cl->addWidget(section_label("Capabilities"));
    capsBar = new QWidget;
    auto* capsLay = new QHBoxLayout(capsBar);
    capsLay->setContentsMargins(0, 0, 0, 0);
    capsLay->setSpacing(6);
    capsLay->addStretch();
    cl->addWidget(capsBar);
    ipl->addWidget(capCard);

    // inner split: exports/imports left, invoke right
    auto* split = new QSplitter(Qt::Horizontal);
    split->setChildrenCollapsible(false);
    split->setObjectName("wbSplit");
    split->setHandleWidth(3);

    // exports/imports tabs card
    auto* eiCard = card();
    auto* eil = new QVBoxLayout(eiCard);
    eil->setContentsMargins(0, 0, 0, 0);
    auto* tabs = new QTabWidget;
    tabs->setObjectName("wbTabs");
    expTable = new QTableWidget(0, 2);
    expTable->setObjectName("dataTable");
    expTable->setHorizontalHeaderLabels({"#", "Name"});
    expTable->horizontalHeader()->setStretchLastSection(true);
    expTable->setColumnWidth(0, 40);
    expTable->verticalHeader()->setVisible(false);
    expTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    expTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    expTable->setShowGrid(false);
    expTable->setAlternatingRowColors(true);
    tabs->addTab(expTable, "Exports");

    impTable = new QTableWidget(0, 2);
    impTable->setObjectName("dataTable");
    impTable->setHorizontalHeaderLabels({"#", "Name"});
    impTable->horizontalHeader()->setStretchLastSection(true);
    impTable->setColumnWidth(0, 40);
    impTable->verticalHeader()->setVisible(false);
    impTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    impTable->setShowGrid(false);
    impTable->setAlternatingRowColors(true);
    tabs->addTab(impTable, "Imports");
    eil->addWidget(tabs);
    split->addWidget(eiCard);

    connect(expTable, &QTableWidget::itemDoubleClicked, this, [this](QTableWidgetItem* it){
        if (it->column() != 1) return;
        callExport->setText(it->text());
    });

    // invoke card
    auto* invCard = card();
    auto* il = new QVBoxLayout(invCard);
    il->setContentsMargins(20, 16, 20, 16);
    il->setSpacing(10);
    il->addWidget(h2("Invoke export"));

    auto* form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight);
    form->setSpacing(8);
    callExport  = new QLineEdit;
    callExport->setPlaceholderText("pick from Exports list, or type name");
    callArgs    = new QLineEdit; callArgs->setPlaceholderText("hex/dec, comma-separated");
    callArgs->setFont(QFont("JetBrainsMono NF", 10));
    callTimeout = new QSpinBox; callTimeout->setRange(0, 60000); callTimeout->setSuffix(" ms");
    form->addRow("Export", callExport);
    form->addRow("Args",   callArgs);
    form->addRow("Timeout", callTimeout);
    il->addLayout(form);

    auto* callBtn = new QPushButton("Call"); callBtn->setObjectName("btnPrimary");
    il->addWidget(callBtn);

    il->addWidget(section_label("Last result"));
    callResult = new QLabel("no calls yet");
    callResult->setObjectName("resultLabel");
    callResult->setTextInteractionFlags(Qt::TextSelectableByMouse);
    il->addWidget(callResult);

    il->addWidget(section_label("Call history"));
    callHist = new QTableWidget(0, 3);
    callHist->setObjectName("dataTable");
    callHist->setHorizontalHeaderLabels({"Time", "Export", "Result"});
    callHist->horizontalHeader()->setStretchLastSection(false);
    callHist->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    callHist->setColumnWidth(0, 80);
    callHist->setColumnWidth(2, 160);
    callHist->verticalHeader()->setVisible(false);
    callHist->setEditTriggers(QAbstractItemView::NoEditTriggers);
    callHist->setShowGrid(false);
    callHist->setAlternatingRowColors(true);
    il->addWidget(callHist, 1);

    split->addWidget(invCard);
    split->setStretchFactor(0, 55);
    split->setStretchFactor(1, 45);
    split->setSizes({560, 440});
    ipl->addWidget(split, 1);

    rcv->addWidget(infoPanel);
    infoPanel->hide();

    mainSplit->addWidget(rightCol);
    mainSplit->setStretchFactor(0, 0);
    mainSplit->setStretchFactor(1, 1);
    mainSplit->setSizes({260, 900});
    root->addWidget(mainSplit, 1);

    connect(callBtn, &QPushButton::clicked, this, [this]{
        if (!current.valid) return;
        QVector<quint64> args;
        for (const QString& tok : callArgs->text().split(',', Qt::SkipEmptyParts)) {
            QString s = tok.trimmed();
            if (s.isEmpty()) continue;
            bool ok = false;
            quint64 v = s.startsWith("0x", Qt::CaseInsensitive)
                ? s.mid(2).toULongLong(&ok, 16)
                : s.toULongLong(&ok, 0);
            if (ok) args.push_back(v);
        }
        quint64 rv = 0; QString err;
        bool ok = drv->call_export(current.base.id, callExport->text(), callTimeout->value(), args, rv, err);
        int row = callHist->rowCount();
        callHist->insertRow(0);
        callHist->setItem(0, 0, new QTableWidgetItem(QDateTime::currentDateTime().toString("HH:mm:ss")));
        callHist->setItem(0, 1, new QTableWidgetItem(callExport->text()));
        QString rs = ok ? QString::asprintf("0x%llx", (unsigned long long)rv) : ("err: " + err);
        auto* ri = new QTableWidgetItem(rs);
        ri->setForeground(ok ? QColor("#ffffff") : QColor("#e06c6c"));
        callHist->setItem(0, 2, ri);
        if (callHist->rowCount() > 100) callHist->removeRow(100);

        if (ok) {
            callResult->setText(QString::asprintf("0x%016llx", (unsigned long long)rv));
            callResult->setStyleSheet("color:#f3f4f6; font-family:'JetBrainsMono NF'; font-size:15px; font-weight:500;");
            emit toast(QString("call ok: 0x%1").arg(rv, 0, 16), 2);
        } else {
            callResult->setText(err);
            callResult->setStyleSheet("color:#e06c6c; font-family:'JetBrainsMono NF'; font-size:13px;");
            emit toast(QString("call failed: %1").arg(err), 1);
        }
        (void)row;
    });
}

void WorkbenchPage::refreshModules(const QVector<ModuleRow>& mods)
{
    // remember which module was selected, restore selection after rebuild
    quint32 prev = current.valid ? current.base.id : (quint32)-1;
    if (pending_select) { prev = pending_select; pending_select = 0; }

    // hide the toolkit guest from this list. it's a GUI-internal module the
    // Explorer tab drives; if the user sees it here alongside their own wasm
    // it just looks like something they didn't load.
    QVector<ModuleRow> visible;
    for (const auto& m : mods) {
        QString base = QFileInfo(m.name).fileName();
        if (base.compare("toolkit.wasm", Qt::CaseInsensitive) == 0) continue;
        visible.push_back(m);
    }

    moduleList->blockSignals(true);
    moduleList->clear();
    for (const auto& m : visible) {
        QString base = QFileInfo(m.name).fileName();
        if (base.isEmpty()) base = m.name;
        auto* it = new QListWidgetItem(QString("%1  %2").arg(m.id, 2, 10, QChar('0')).arg(base));
        it->setData(Qt::UserRole, m.id);
        it->setToolTip(QString("%1\nhash %2\nwasm %3 B, mem %4 KB, pool %5 B")
            .arg(m.name)
            .arg(m.hash, 16, 16, QChar('0'))
            .arg(m.wasm_size).arg(m.mem_pages * 64).arg(m.pool_bytes));
        moduleList->addItem(it);
        if (m.id == prev) moduleList->setCurrentItem(it);
    }
    moduleList->blockSignals(false);
    moduleCount->setText(QString("(%1)").arg(visible.size()));

    if (visible.isEmpty()) clearSelection();
    else if (prev != (quint32)-1) {
        // trigger the same signal path so info reloads even if selection didn't change
        for (int i = 0; i < moduleList->count(); i++) {
            if (moduleList->item(i)->data(Qt::UserRole).toUInt() == prev) {
                emit moduleSelected(prev);
                break;
            }
        }
    }
}

void WorkbenchPage::setSelected(quint32 id, const ModuleInfo& info)
{
    (void)id;
    current = info;
    if (!info.valid) { clearSelection(); return; }

    emptyPanel->hide();
    infoPanel->show();

    modTitle->setText(QFileInfo(info.base.name).fileName().isEmpty() ? info.base.name : QFileInfo(info.base.name).fileName());
    modMeta->setText(QString::asprintf("id %u   %016llx   %.1f KB wasm   %u KB mem   %llu B pool",
        info.base.id, (unsigned long long)info.base.hash, info.base.wasm_size/1024.0,
        info.base.mem_pages*64, (unsigned long long)info.base.pool_bytes));

    refreshCaps(info.caps);

    // filter out internal exports (__gvm_caps) so the user sees only what they wrote
    QStringList user_exports;
    for (const auto& e : info.exports) if (!e.startsWith("__gvm_")) user_exports << e;

    expTable->setRowCount(user_exports.size());
    QFont mono("JetBrainsMono NF", 10);
    for (int i = 0; i < user_exports.size(); i++) {
        auto* n = new QTableWidgetItem(QString::number(i)); n->setFont(mono); n->setForeground(QColor("#7b818c"));
        expTable->setItem(i, 0, n);
        auto* e = new QTableWidgetItem(user_exports[i]); e->setFont(mono);
        expTable->setItem(i, 1, e);
    }
    impTable->setRowCount(info.imports.size());
    for (int i = 0; i < info.imports.size(); i++) {
        auto* n = new QTableWidgetItem(QString::number(i)); n->setFont(mono); n->setForeground(QColor("#7b818c"));
        impTable->setItem(i, 0, n);
        auto* e = new QTableWidgetItem(info.imports[i]); e->setFont(mono); e->setForeground(QColor("#a8adb8"));
        impTable->setItem(i, 1, e);
    }

    // auto-populate export field with first user export, but don't clobber
    // whatever the user typed unless it doesn't match anything from this module
    if (!user_exports.isEmpty() && !user_exports.contains(callExport->text()))
        callExport->setText(user_exports.first());

    // highlight the corresponding row in the list without re-emitting signals
    moduleList->blockSignals(true);
    for (int i = 0; i < moduleList->count(); i++) {
        if (moduleList->item(i)->data(Qt::UserRole).toUInt() == info.base.id) {
            moduleList->setCurrentRow(i);
            break;
        }
    }
    moduleList->blockSignals(false);
}

void WorkbenchPage::clearSelection()
{
    current = {};
    infoPanel->hide();
    emptyPanel->show();
}

void WorkbenchPage::refreshCaps(quint32 caps)
{
    auto* lay = static_cast<QHBoxLayout*>(capsBar->layout());
    while (auto* it = lay->takeAt(0)) {
        if (it->widget()) it->widget()->deleteLater();
        delete it;
    }
    for (int i = 0; i < 10; i++) {
        bool on = (caps >> i) & 1;
        lay->addWidget(pill(CAP_NAMES[i], on ? "on" : "off"));
    }
    lay->addStretch();
}

// MemoryPage

MemoryPage::MemoryPage(DriverClient* d, QWidget* p) : QWidget(p), drv(d)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 8);
    root->setSpacing(14);

    auto* c = card();
    auto* cl = new QVBoxLayout(c);
    cl->setContentsMargins(20, 16, 20, 16);
    cl->setSpacing(10);
    cl->addWidget(h2("Read"));

    auto* form = new QFormLayout;
    form->setSpacing(8);
    modId  = new QSpinBox; modId->setRange(-1, 999); modId->setValue(-1); modId->setSpecialValueText("(not set)");
    exp    = new QLineEdit; exp->setText("toolkit_read_kmem");
    addr   = new QLineEdit; addr->setText("0xfffff800`00000000"); addr->setFont(QFont("JetBrainsMono NF", 10));
    length = new QSpinBox; length->setRange(16, 4096); length->setValue(256); length->setSuffix(" bytes");
    form->addRow("Toolkit module id", modId);
    form->addRow("Export",            exp);
    form->addRow("Address",           addr);
    form->addRow("Length",            length);
    cl->addLayout(form);

    auto* readBtn = new QPushButton("Read"); readBtn->setObjectName("btnPrimary");
    cl->addWidget(readBtn, 0, Qt::AlignLeft);
    status = new QLabel(""); status->setObjectName("dim");
    cl->addWidget(status);
    root->addWidget(c);

    hex = new QPlainTextEdit;
    hex->setReadOnly(true);
    hex->setFont(QFont("JetBrainsMono NF", 10));
    hex->setObjectName("hexView");
    hex->setPlaceholderText("Hex output will appear here after a successful read.");
    root->addWidget(hex, 1);

    connect(readBtn, &QPushButton::clicked, this, [this]{
        status->clear();
        if (modId->value() < 0) { status->setText("set toolkit mod id first"); return; }
        QString a = addr->text(); a.remove('`').remove(' ');
        bool ok = false;
        quint64 va = a.startsWith("0x", Qt::CaseInsensitive) ? a.mid(2).toULongLong(&ok, 16) : a.toULongLong(&ok, 0);
        if (!ok) { status->setText("bad address"); return; }
        QVector<quint64> argv = { va, 0, (quint64)length->value() };
        quint64 rv = 0; QString err;
        if (drv->call_export(modId->value(), exp->text(), 2000, argv, rv, err))
            status->setText(QString::asprintf("toolkit rv=0x%llx bytes read. hex readback needs the new IOCTL.", (unsigned long long)rv));
        else
            status->setText("call failed: " + err);
    });
}

// EventsPage

static const char* evt_tag(int k)
{
    switch (k) {
    case EVT_PROC: return "PROC"; case EVT_EXIT: return "EXIT"; case EVT_IMAGE: return "IMAGE";
    case EVT_CALL: return "CALL"; case EVT_LOAD: return "LOAD"; case EVT_UNLOAD: return "UNLD";
    case EVT_ERROR: return "ERR"; default: return "MSG";
    }
}

static QColor evt_col(int k)
{
    switch (k) {
    case EVT_PROC: case EVT_LOAD:  return QColor("#8fbf5c");
    case EVT_EXIT: case EVT_ERROR: return QColor("#cd5c5c");
    case EVT_IMAGE:                return QColor("#7cb1f0");
    case EVT_CALL:                 return QColor("#d4a259");
    case EVT_UNLOAD:               return QColor("#b96060");
    default:                       return QColor("#d4d4d4");
    }
}

EventsPage::EventsPage(DriverClient* d, QWidget* p) : QWidget(p), drv(d)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 8);
    root->setSpacing(10);

    // control card: load + start + stop + status
    auto* ctlCard = card();
    auto* cl = new QHBoxLayout(ctlCard);
    cl->setContentsMargins(20, 12, 20, 12);
    cl->setSpacing(8);
    loadBtn      = new QPushButton("Load tracer");
    loadBtn->setToolTip("Load samples/process_tracer.wasm");
    startBtn     = new QPushButton("Start (poll)");
    startBtn->setToolTip("Enable callbacks. Events log via driver notify handlers.");
    reactiveBtn  = new QPushButton("Start reactive");
    reactiveBtn->setObjectName("btnPrimary");
    reactiveBtn->setToolTip("Enable callbacks and spawn kernel dispatch worker that invokes guest exports.");
    stopBtn      = new QPushButton("Stop");
    stopBtn->setObjectName("btnDanger");
    stopBtn->setToolTip("Unload the tracer module (removes registered callbacks)");
    statusLbl    = new QLabel;
    cl->addWidget(loadBtn);
    cl->addWidget(startBtn);
    cl->addWidget(reactiveBtn);
    cl->addWidget(stopBtn);
    cl->addSpacing(8);
    cl->addWidget(statusLbl, 1);
    root->addWidget(ctlCard);

    set_status("no tracer loaded", "#8b8b8b");
    startBtn->setEnabled(false);
    reactiveBtn->setEnabled(false);
    stopBtn->setEnabled(false);

    connect(loadBtn,     &QPushButton::clicked, this, &EventsPage::on_load_tracer);
    connect(startBtn,    &QPushButton::clicked, this, [this]{ on_start(false); });
    connect(reactiveBtn, &QPushButton::clicked, this, [this]{ on_start(true);  });
    connect(stopBtn,     &QPushButton::clicked, this, &EventsPage::on_stop);

    // filter bar
    auto* barCard = card();
    auto* bl = new QHBoxLayout(barCard);
    bl->setContentsMargins(20, 12, 20, 12);
    bl->setSpacing(12);
    cbProc  = new QCheckBox("Proc");  cbProc->setChecked(true);
    cbExit  = new QCheckBox("Exit");  cbExit->setChecked(true);
    cbImage = new QCheckBox("Image"); cbImage->setChecked(true);
    cbCall  = new QCheckBox("Call");  cbCall->setChecked(true);
    cbError = new QCheckBox("Error"); cbError->setChecked(true);
    filter = new QLineEdit; filter->setPlaceholderText("Substring filter"); filter->setFixedWidth(260);
    bl->addWidget(cbProc); bl->addWidget(cbExit); bl->addWidget(cbImage); bl->addWidget(cbCall); bl->addWidget(cbError);
    bl->addStretch();
    bl->addWidget(filter);
    root->addWidget(barCard);

    table = new QTableWidget(0, 3);
    table->setObjectName("dataTable");
    table->setHorizontalHeaderLabels({"Time", "Kind", "Line"});
    table->horizontalHeader()->setStretchLastSection(true);
    table->setColumnWidth(0, 110);
    table->setColumnWidth(1, 80);
    table->verticalHeader()->setVisible(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setShowGrid(false);
    table->setAlternatingRowColors(true);
    root->addWidget(table, 1);
}

void EventsPage::append(const QVector<LogEntry>& entries)
{
    for (const auto& e : entries) {
        bool show =
            (e.kind == EVT_PROC  && cbProc->isChecked())  ||
            (e.kind == EVT_EXIT  && cbExit->isChecked())  ||
            (e.kind == EVT_IMAGE && cbImage->isChecked()) ||
            ((e.kind == EVT_CALL || e.kind == EVT_LOAD || e.kind == EVT_UNLOAD) && cbCall->isChecked()) ||
            (e.kind == EVT_ERROR && cbError->isChecked());
        if (!show) continue;
        QString f = filter->text();
        if (!f.isEmpty() && !e.line.contains(f)) continue;

        table->insertRow(0);
        auto* ts = new QTableWidgetItem(e.ts);
        ts->setFont(QFont("JetBrainsMono NF", 10));
        ts->setForeground(QColor("#8a8a8a"));
        table->setItem(0, 0, ts);
        auto* kd = new QTableWidgetItem(evt_tag(e.kind));
        kd->setFont(QFont("JetBrainsMono NF", 10));
        kd->setForeground(evt_col(e.kind));
        table->setItem(0, 1, kd);
        auto* ln = new QTableWidgetItem(e.line);
        ln->setFont(QFont("JetBrainsMono NF", 10));
        table->setItem(0, 2, ln);
        if (table->rowCount() > 2000) table->removeRow(2000);
    }
}

void EventsPage::clear() { table->setRowCount(0); }

void EventsPage::set_status(const QString& s, const QString& color)
{
    statusLbl->setText(s);
    statusLbl->setStyleSheet(QString("color:%1; font-size:12px;").arg(color));
}

void EventsPage::on_load_tracer()
{
    QString here = QCoreApplication::applicationDirPath();
    QStringList candidates = {
        here + "/../features/process_tracer.wasm",
        here + "/features/process_tracer.wasm",
        here + "/process_tracer.wasm",
        here + "/../samples/process_tracer.wasm",
    };
    QString path;
    for (const auto& p : candidates) {
        if (QFile::exists(p)) { path = p; break; }
    }
    if (path.isEmpty()) {
        path = QFileDialog::getOpenFileName(this, "Select process_tracer.wasm",
            QString(), "WebAssembly (*.wasm)");
        if (path.isEmpty()) return;
    }
    quint32 id = 0; QString err;
    if (!drv->load_module(path, 0, id, err)) {
        set_status(QString("load failed: %1").arg(err), "#e06c6c");
        emit toast("load failed: " + err, 1);
        return;
    }
    tracer_id = (int)id;
    set_status(QString("tracer loaded as module %1").arg(id), "#7fb069");
    startBtn->setEnabled(true);
    reactiveBtn->setEnabled(true);
    stopBtn->setEnabled(true);
    loadBtn->setEnabled(false);
    emit toast(QString("tracer loaded (module %1)").arg(id), 2);
}

void EventsPage::on_start(bool reactive)
{
    if (tracer_id < 0) { emit toast("load tracer first", 1); return; }
    const char* fn = reactive ? "start_reactive" : "start";
    quint64 rv = 0; QString err;
    if (drv->call_export((quint32)tracer_id, fn, 2000, {}, rv, err)) {
        running = true;
        set_status(QString("%1 running (module %2)").arg(reactive ? "reactive dispatch" : "callbacks").arg(tracer_id), "#7fb069");
        emit toast(QString("%1 on").arg(reactive ? "reactive" : "polling"), 2);
    } else {
        set_status("start failed: " + err, "#e06c6c");
        emit toast("start failed: " + err, 1);
    }
}

void EventsPage::on_stop()
{
    // stop dispatch worker + remove kernel callbacks first, THEN unload the
    // module. otherwise callbacks keep firing after the module is gone.
    drv->notify_stop();
    if (tracer_id >= 0) drv->unload((quint32)tracer_id);
    tracer_id = -1;
    running   = false;
    set_status("stopped", "#8b8b8b");
    loadBtn->setEnabled(true);
    startBtn->setEnabled(false);
    reactiveBtn->setEnabled(false);
    stopBtn->setEnabled(false);
    emit toast("callbacks removed, tracer unloaded", 0);
}

// LogPage

LogPage::LogPage(QWidget* p) : QWidget(p)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 8);
    root->setSpacing(14);

    auto* bar = new QHBoxLayout;
    auto* clearBtn = new QPushButton("Clear");
    autoscroll = new QCheckBox("Auto-scroll"); autoscroll->setChecked(true);
    statusLbl = new QLabel(""); statusLbl->setObjectName("dim");
    bar->addWidget(clearBtn);
    bar->addWidget(autoscroll);
    bar->addStretch();
    bar->addWidget(statusLbl);
    root->addLayout(bar);

    view = new QPlainTextEdit;
    view->setReadOnly(true);
    view->setFont(QFont("JetBrainsMono NF", 10));
    view->setObjectName("logView");
    view->setMaximumBlockCount(4000);
    root->addWidget(view, 1);

    connect(clearBtn, &QPushButton::clicked, this, [this]{ view->clear(); });
}

void LogPage::append(const QVector<LogEntry>& entries, quint64 seq, quint32 dropped)
{
    for (const auto& e : entries) {
        QString col = evt_col(e.kind).name();
        QString html = QString("<span style='color:#6a6a6a'>%1</span> <span style='color:%2'>%3</span>")
            .arg(e.ts, col, e.line.toHtmlEscaped());
        view->appendHtml(html);
    }
    statusLbl->setText(QString("seq %1 | %2 dropped").arg(seq).arg(dropped));
    if (autoscroll->isChecked()) {
        auto* sb = view->verticalScrollBar();
        sb->setValue(sb->maximum());
    }
}

void LogPage::clear() { view->clear(); }

// ProfilerPage

ProfilerPage::ProfilerPage(QWidget* p) : QWidget(p)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 8);
    root->setSpacing(12);

    auto* row = new QHBoxLayout;
    auto* clr = new QPushButton("Reset counts");
    total = new QLabel("no data yet");
    total->setStyleSheet("color:#7b828e; font-size:12px;");
    row->addWidget(clr); row->addWidget(total, 1);
    root->addLayout(row);

    table = new QTableWidget(0, 5);
    table->setObjectName("dataTable");
    table->setHorizontalHeaderLabels({"Function", "Count", "Total", "Max", "Avg"});
    table->horizontalHeader()->setStretchLastSection(false);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->setColumnWidth(1, 100);
    table->setColumnWidth(2, 140);
    table->setColumnWidth(3, 140);
    table->setColumnWidth(4, 140);
    table->verticalHeader()->setVisible(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setShowGrid(false);
    table->setAlternatingRowColors(true);
    table->setSortingEnabled(true);
    root->addWidget(table, 1);

    connect(clr, &QPushButton::clicked, this, &ProfilerPage::clear);
}

static QString fmt_time_ns(quint64 ns)
{
    if (ns < 1000)          return QString::asprintf("%llu ns", (unsigned long long)ns);
    if (ns < 1000 * 1000)   return QString::asprintf("%.2f us", ns / 1000.0);
    if (ns < 1000ULL*1000*1000) return QString::asprintf("%.2f ms", ns / 1e6);
    return QString::asprintf("%.2f s", ns / 1e9);
}

void ProfilerPage::observe(const QVector<TraceEvent>& events)
{
    for (const auto& e : events) {
        auto& s = stats[e.name];
        s.count++;
        if (e.kind == TRK_CALL) {
            s.last_call_ts = e.timestamp_100ns;
        } else if (e.kind == TRK_RETURN && s.last_call_ts && e.timestamp_100ns >= s.last_call_ts) {
            quint64 delta_ns = (e.timestamp_100ns - s.last_call_ts) * 100;  // 100ns ticks -> ns
            s.total_ns += delta_ns;
            if (delta_ns > s.max_ns) s.max_ns = delta_ns;
            s.last_call_ts = 0;
        }
    }
    refresh();
}

void ProfilerPage::refresh()
{
    table->setSortingEnabled(false);
    table->setRowCount(stats.size());
    QFont mono("JetBrainsMono NF", 10);
    int r = 0; quint64 grand = 0;
    for (auto it = stats.constBegin(); it != stats.constEnd(); ++it, r++) {
        const auto& s = it.value();
        grand += s.count;
        auto* n = new QTableWidgetItem(it.key()); n->setFont(mono);
        table->setItem(r, 0, n);
        auto* c = new QTableWidgetItem;
        c->setData(Qt::DisplayRole, (qulonglong)s.count);
        c->setFont(mono);
        table->setItem(r, 1, c);
        auto* t = new QTableWidgetItem(s.total_ns ? fmt_time_ns(s.total_ns) : "");
        t->setData(Qt::UserRole, (qulonglong)s.total_ns);
        t->setFont(mono); t->setForeground(QColor("#ffffff"));
        table->setItem(r, 2, t);
        auto* mx = new QTableWidgetItem(s.max_ns ? fmt_time_ns(s.max_ns) : "");
        mx->setFont(mono);
        table->setItem(r, 3, mx);
        quint64 avg = s.count ? s.total_ns / s.count : 0;
        auto* av = new QTableWidgetItem(avg ? fmt_time_ns(avg) : "");
        av->setFont(mono); av->setForeground(QColor("#8a919e"));
        table->setItem(r, 4, av);
    }
    table->setSortingEnabled(true);
    table->sortItems(1, Qt::DescendingOrder);
    total->setText(QString("%1 unique functions | %2 events total").arg(stats.size()).arg(grand));
}

void ProfilerPage::clear() { stats.clear(); table->setRowCount(0); total->setText("no data yet"); }

// DeployPage

DeployPage::DeployPage(DriverClient* d, QWidget* p) : QWidget(p), drv(d)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 8);
    root->setSpacing(14);

    auto* stateCard = card();
    auto* sl = new QVBoxLayout(stateCard);
    sl->setContentsMargins(20, 16, 20, 16);
    sl->setSpacing(10);
    sl->addWidget(section_label("Status"));

    auto* g = new QGridLayout;
    g->setHorizontalSpacing(30);
    g->setVerticalSpacing(10);
    auto mklbl = [](const QString& k){ auto* l = new QLabel(k); l->setStyleSheet("color:#8a8a8a;"); return l; };
    g->addWidget(mklbl("service"),  0, 0);
    svcState = new QLabel("-"); g->addWidget(svcState, 0, 1);
    g->addWidget(mklbl("state"),    1, 0);
    runState = new QLabel("-"); g->addWidget(runState, 1, 1);
    g->addWidget(mklbl("driver"),   2, 0);
    devState = new QLabel("-"); g->addWidget(devState, 2, 1);
    sl->addLayout(g);
    root->addWidget(stateCard);

    auto* actCard = card();
    auto* al = new QVBoxLayout(actCard);
    al->setContentsMargins(20, 16, 20, 16);
    al->setSpacing(10);
    al->addWidget(section_label("Actions"));
    auto* btns = new QHBoxLayout;
    auto* install = new QPushButton("Install");
    auto* start   = new QPushButton("Start"); start->setObjectName("btnPrimary");
    auto* stop    = new QPushButton("Stop");
    auto* remove  = new QPushButton("Remove"); remove->setObjectName("btnDanger");
    auto* refresh = new QPushButton("Refresh");
    btns->addWidget(install); btns->addWidget(start); btns->addWidget(stop);
    btns->addWidget(remove);  btns->addStretch();     btns->addWidget(refresh);
    al->addLayout(btns);
    root->addWidget(actCard);

    root->addStretch();

    connect(install, &QPushButton::clicked, this, [this]{
        wchar_t path[MAX_PATH]; GetModuleFileNameW(nullptr, path, MAX_PATH);
        QString exe = QString::fromWCharArray(path);
        QString dir = QFileInfo(exe).absolutePath();
        drv->run_sc(QString("create Goodmans type= kernel binPath= \"%1\\Goodmans.sys\"").arg(dir));
        emit stateChanged();
    });
    connect(start,   &QPushButton::clicked, this, [this]{ drv->run_sc("start Goodmans");  emit stateChanged(); });
    connect(stop,    &QPushButton::clicked, this, [this]{ drv->run_sc("stop Goodmans");   emit stateChanged(); });
    connect(remove,  &QPushButton::clicked, this, [this]{
        drv->run_sc("stop Goodmans"); drv->run_sc("delete Goodmans"); emit stateChanged();
    });
    connect(refresh, &QPushButton::clicked, this, [this]{ emit stateChanged(); });
}

void DeployPage::refresh(bool devUp, bool present, bool running)
{
    auto set = [](QLabel* l, const QString& t, const QString& color){
        l->setText(t);
        l->setStyleSheet(QString("color:%1; font-weight:600;").arg(color));
    };
    set(svcState, present ? "installed" : "not installed", present ? "#8fbf5c" : "#cd5c5c");
    set(runState, running ? "running"   : "stopped",       running ? "#8fbf5c" : "#e5b055");
    set(devState, devUp   ? "loaded"    : "not loaded",    devUp   ? "#8fbf5c" : "#cd5c5c");
}

// TracePage

static const char* trace_kind_str(quint32 k)
{
    switch (k) {
    case TRK_IMPORT: return "IMPORT";
    case TRK_CALL:   return "CALL";
    case TRK_RETURN: return "RETURN";
    case TRK_TRAP:   return "TRAP";
    default:         return "?";
    }
}

static QColor trace_kind_col(quint32 k)
{
    switch (k) {
    case TRK_IMPORT: return QColor("#7cb1f0");
    case TRK_CALL:   return QColor("#d4a259");
    case TRK_RETURN: return QColor("#8fbf5c");
    case TRK_TRAP:   return QColor("#cd5c5c");
    default:         return QColor("#8a8a8a");
    }
}

// argument name hints per host import so details pane can label them properly.
// values are decoded per column type: 'A' = kernel address hex, 'X' = hex u64,
// 'x' = hex u32, 'd' = decimal u32.
struct ArgSpec { const char* fn; const char* names[4]; const char  types[4]; };
static const ArgSpec ARG_SPECS[] = {
    { "host_read_u8",         {"kaddr",        "", "",       ""},        {'A','\0','\0','\0'} },
    { "host_read_u32",        {"kaddr",        "", "",       ""},        {'A','\0','\0','\0'} },
    { "host_read_u64",        {"kaddr",        "", "",       ""},        {'A','\0','\0','\0'} },
    { "host_write_u64",       {"kaddr",        "value",  "", ""},        {'A','X','\0','\0'} },
    { "host_read_bytes",      {"kaddr",        "guest_off", "len",  ""}, {'A','x','d','\0'}   },
    { "host_write_bytes",     {"kaddr",        "guest_off", "len",  ""}, {'A','x','d','\0'}   },
    { "host_alloc",           {"size",         "", "",       ""},        {'d','\0','\0','\0'} },
    { "host_free",            {"kernel_va",    "", "",       ""},        {'A','\0','\0','\0'} },
    { "host_dbg_print",       {"guest_off",    "len",     "", ""},       {'x','d','\0','\0'}  },
    { "host_readmsr",         {"msr",          "", "",       ""},        {'x','\0','\0','\0'} },
    { "host_writemsr",        {"msr",          "value",  "", ""},        {'x','X','\0','\0'}  },
    { "host_cpuid",           {"leaf",         "sub",   "guest_off",""}, {'x','x','x','\0'}   },
    { "host_phys_read",       {"pa",           "guest_off","len",   ""}, {'X','x','d','\0'}   },
    { "host_phys_write",      {"pa",           "guest_off","len",   ""}, {'X','x','d','\0'}   },
    { "host_rdtsc",           {"","","",""},                              {'\0','\0','\0','\0'}},
    { "host_current_irql",    {"","","",""},                              {'\0','\0','\0','\0'}},
    { "host_process_id",      {"","","",""},                              {'\0','\0','\0','\0'}},
    { "host_thread_id",       {"","","",""},                              {'\0','\0','\0','\0'}},
    { "host_current_process", {"","","",""},                              {'\0','\0','\0','\0'}},
    { nullptr, {}, {} }
};

static const ArgSpec* find_argspec(const QString& fn)
{
    for (int i = 0; ARG_SPECS[i].fn; i++)
        if (fn == ARG_SPECS[i].fn) return &ARG_SPECS[i];
    return nullptr;
}

static QString fmt_arg_val(char type, quint64 v)
{
    switch (type) {
    case 'A': // kernel address split for readability
        return QString::asprintf("0x%08x`%08x", (quint32)(v >> 32), (quint32)v);
    case 'X': return QString::asprintf("0x%016llx", (unsigned long long)v);
    case 'x': return QString::asprintf("0x%x",     (quint32)v);
    case 'd': return QString::number((quint32)v);
    default:  return QString::asprintf("0x%llx",   (unsigned long long)v);
    }
}

static QString short_args_display(const TraceEvent& e)
{
    const ArgSpec* spec = find_argspec(e.name);
    if (!spec) {
        QString s;
        for (int i = 0; i < e.argv.size(); i++) {
            if (i) s += ", ";
            s += QString::asprintf("0x%llx", (unsigned long long)e.argv[i]);
        }
        return s;
    }
    QString s;
    for (int i = 0; i < e.argv.size() && spec->types[i]; i++) {
        if (i) s += ", ";
        s += fmt_arg_val(spec->types[i], e.argv[i]);
    }
    return s;
}

TracePage::TracePage(DriverClient* d, QWidget* p) : QWidget(p), drv(d)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 8);
    root->setSpacing(12);

    // toolbar
    auto* toolbar = new QFrame;
    toolbar->setObjectName("toolbar");
    auto* tb = new QHBoxLayout(toolbar);
    tb->setContentsMargins(14, 8, 14, 8);
    tb->setSpacing(8);

    toggleBtn = new QPushButton("Start");
    toggleBtn->setObjectName("btnPrimary");
    toggleBtn->setMinimumWidth(80);
    toggleBtn->setShortcut(QKeySequence("F5"));
    toggleBtn->setToolTip("Toggle tracing (F5)");
    tb->addWidget(toggleBtn);

    pauseBtn = new QPushButton("Pause");
    pauseBtn->setEnabled(false);
    pauseBtn->setShortcut(QKeySequence("F6"));
    pauseBtn->setToolTip("Pause UI capture (F6)");
    tb->addWidget(pauseBtn);

    clearBtn = new QPushButton("Clear");
    clearBtn->setShortcut(QKeySequence("Ctrl+L"));
    clearBtn->setToolTip("Clear view (Ctrl+L)");
    tb->addWidget(clearBtn);

    openBtn = new QPushButton("Open…");
    openBtn->setToolTip("Load .gtrace file");
    tb->addWidget(openBtn);

    exportBtn = new QPushButton("Save…");
    exportBtn->setShortcut(QKeySequence("Ctrl+S"));
    exportBtn->setToolTip("Save trace to .gtrace or .csv (Ctrl+S)");
    tb->addWidget(exportBtn);

    auto* sep1 = new QFrame; sep1->setFrameShape(QFrame::VLine); sep1->setObjectName("tbsep");
    sep1->setFixedHeight(24);
    tb->addWidget(sep1);

    onlyModule = new QCheckBox("only");
    onlyModule->setToolTip("Limit tracing to a specific module id");
    moduleFilter = new QSpinBox;
    moduleFilter->setRange(0, 999);
    moduleFilter->setValue(0);
    moduleFilter->setFixedWidth(60);
    tb->addWidget(onlyModule);
    tb->addWidget(moduleFilter);

    regexMode = new QCheckBox(".*");
    regexMode->setToolTip("Treat filter as regex");
    tb->addWidget(regexMode);

    auto* sep2 = new QFrame; sep2->setFrameShape(QFrame::VLine); sep2->setObjectName("tbsep");
    sep2->setFixedHeight(24);
    tb->addWidget(sep2);

    filter = new QLineEdit;
    filter->setPlaceholderText("filter (Ctrl+F): substring, module id, kind, or regex");
    filter->setClearButtonEnabled(true);
    tb->addWidget(filter, 1);

    root->addWidget(toolbar);

    // splitter: table above, details below
    auto* split = new QSplitter(Qt::Vertical);
    split->setObjectName("wbSplit");
    split->setChildrenCollapsible(false);
    split->setHandleWidth(4);

    table = new QTableWidget(0, 9);
    table->setObjectName("dataTable");
    table->setHorizontalHeaderLabels({"", "Time", "Δt", "TID", "IRQL", "Mod", "Kind", "Function", "Arguments"});
    table->horizontalHeader()->setStretchLastSection(true);
    table->setColumnWidth(0, 14);
    table->setColumnWidth(1, 110);
    table->setColumnWidth(2, 80);
    table->setColumnWidth(3, 60);
    table->setColumnWidth(4, 50);
    table->setColumnWidth(5, 46);
    table->setColumnWidth(6, 78);
    table->setColumnWidth(7, 200);
    table->verticalHeader()->setVisible(false);
    table->verticalHeader()->setDefaultSectionSize(24);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setShowGrid(false);
    table->setAlternatingRowColors(true);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    split->addWidget(table);

    details = new QPlainTextEdit;
    details->setReadOnly(true);
    details->setObjectName("detailsPane");
    details->setPlaceholderText("Select an event to see its full details.");
    details->setMinimumHeight(120);
    split->addWidget(details);
    split->setStretchFactor(0, 70);
    split->setStretchFactor(1, 30);
    split->setSizes({520, 200});
    root->addWidget(split, 1);

    // status bar at bottom
    auto* statusRow = new QHBoxLayout;
    statusRow->setContentsMargins(4, 0, 4, 0);
    modeLbl = new QLabel; modeLbl->setObjectName("dim");
    statsLbl = new QLabel; statsLbl->setObjectName("dim");
    statusRow->addWidget(modeLbl);
    statusRow->addStretch();
    statusRow->addWidget(statsLbl);
    root->addLayout(statusRow);

    refresh_stats();

    connect(toggleBtn, &QPushButton::clicked, this, &TracePage::toggleTracing);
    connect(pauseBtn,  &QPushButton::clicked, this, [this]{
        paused = !paused; pauseBtn->setText(paused ? "Resume" : "Pause");
    });
    connect(clearBtn,  &QPushButton::clicked, this, &TracePage::clear);

    connect(openBtn, &QPushButton::clicked, this, [this]{
        QString path = QFileDialog::getOpenFileName(this, "Load trace", QString(), "Goodmans trace (*.gtrace);;All (*.*)");
        if (!path.isEmpty()) loadTraceFile(path);
    });
    connect(exportBtn, &QPushButton::clicked, this, [this]{
        QString path = QFileDialog::getSaveFileName(this, "Save trace", "trace.gtrace",
            "Goodmans trace (*.gtrace);;CSV (*.csv)");
        if (!path.isEmpty()) saveTraceFile(path);
    });
    connect(filter, &QLineEdit::textChanged, this, [this]{ rebuild_from_backing(); });
    connect(regexMode, &QCheckBox::toggled, this, [this]{ rebuild_from_backing(); });

    connect(table, &QTableWidget::itemSelectionChanged, this, [this]{
        auto sel = table->selectedItems();
        if (sel.isEmpty()) { details->clear(); return; }
        int row = sel.first()->row();
        int back_idx = table->item(row, 0)->data(Qt::UserRole).toInt();
        if (back_idx >= 0 && back_idx < backing.size()) render_details(backing[back_idx]);
    });

    auto* bmSc = new QShortcut(QKeySequence("Ctrl+B"), this);
    connect(bmSc, &QShortcut::activated, this, &TracePage::toggleBookmark);
}

void TracePage::toggleTracing()
{
    if (!tracing_on) {
        int mode = onlyModule->isChecked() ? TRACE_ON_MODULE : TRACE_ON_ALL;
        quint32 mid = onlyModule->isChecked() ? (quint32)moduleFilter->value() : 0;
        if (drv->trace_ctl(mode, mid)) {
            tracing_on = true;
            toggleBtn->setText("Stop");
            pauseBtn->setEnabled(true);
            modeLbl->setText(onlyModule->isChecked()
                ? QString("tracing module %1").arg(mid)
                : "tracing all modules");
            modeLbl->setStyleSheet("color: #8fbf5c;");
            emit toast("tracing on", 2);
        } else emit toast("failed to enable trace", 1);
    } else {
        if (drv->trace_ctl(TRACE_OFF)) {
            tracing_on = false;
            toggleBtn->setText("Start");
            pauseBtn->setEnabled(false);
            paused = false;
            pauseBtn->setText("Pause");
            modeLbl->setText("tracing off");
            modeLbl->setStyleSheet("color: #6a7280;");
            emit toast("tracing off", 0);
        }
    }
}

void TracePage::toggleBookmark()
{
    auto sel = table->selectedItems();
    if (sel.isEmpty()) return;
    int row = sel.first()->row();
    int back_idx = table->item(row, 0)->data(Qt::UserRole).toInt();
    if (bookmarks.contains(back_idx)) { bookmarks.remove(back_idx); table->item(row, 0)->setText(""); }
    else                              { bookmarks.insert(back_idx);  table->item(row, 0)->setText("●"); }
}

bool TracePage::filter_match(const TraceEvent& e) const
{
    QString f = filter->text();
    if (f.isEmpty()) return true;
    if (regexMode->isChecked()) {
        QRegularExpression re(f, QRegularExpression::CaseInsensitiveOption);
        if (!re.isValid()) return true; // don't hide during typing
        return re.match(e.name).hasMatch()
            || re.match(QString::number(e.module_id)).hasMatch()
            || re.match(trace_kind_str(e.kind)).hasMatch();
    }
    return e.name.contains(f, Qt::CaseInsensitive)
        || QString::number(e.module_id) == f
        || QString(trace_kind_str(e.kind)).contains(f, Qt::CaseInsensitive);
}

void TracePage::insert_row(int row, const TraceEvent& e, int back_idx)
{
    QFont mono("JetBrainsMono NF", 10);
    table->insertRow(row);

    auto* bm = new QTableWidgetItem(bookmarks.contains(back_idx) ? "●" : "");
    bm->setForeground(QColor("#ffffff"));
    bm->setTextAlignment(Qt::AlignCenter);
    bm->setData(Qt::UserRole, back_idx);
    table->setItem(row, 0, bm);

    auto* ts = new QTableWidgetItem(e.ts); ts->setFont(mono); ts->setForeground(QColor("#8a919e"));
    table->setItem(row, 1, ts);

    // delta-time: microseconds since first event
    quint64 delta_100ns = (first_ts && e.timestamp_100ns >= first_ts) ? e.timestamp_100ns - first_ts : 0;
    double delta_ms = delta_100ns / 10000.0;
    QString dt = delta_ms < 1.0
        ? QString::asprintf("+%.1f us", delta_100ns / 10.0)
        : (delta_ms < 1000.0 ? QString::asprintf("+%.2f ms", delta_ms)
                             : QString::asprintf("+%.3f s",  delta_ms / 1000.0));
    auto* dtc = new QTableWidgetItem(dt); dtc->setFont(mono); dtc->setForeground(QColor("#7b828e"));
    table->setItem(row, 2, dtc);

    auto* tid = new QTableWidgetItem(QString::number(e.thread_id));
    tid->setFont(mono); tid->setForeground(QColor("#8a919e"));
    table->setItem(row, 3, tid);

    QString irqlStr;
    switch (e.irql) {
    case 0: irqlStr = "PASSIVE"; break;
    case 1: irqlStr = "APC";     break;
    case 2: irqlStr = "DISPATCH"; break;
    default: irqlStr = QString::number(e.irql);
    }
    auto* iq = new QTableWidgetItem(irqlStr); iq->setFont(mono);
    iq->setForeground(e.irql >= 2 ? QColor("#e5b055") : QColor("#8a919e"));
    table->setItem(row, 4, iq);

    auto* mid = new QTableWidgetItem(QString::number(e.module_id)); mid->setFont(mono);
    table->setItem(row, 5, mid);

    auto* kd = new QTableWidgetItem(trace_kind_str(e.kind)); kd->setFont(mono);
    kd->setForeground(trace_kind_col(e.kind));
    table->setItem(row, 6, kd);

    auto* nm = new QTableWidgetItem(e.name); nm->setFont(mono);
    table->setItem(row, 7, nm);

    QString argstr = short_args_display(e);
    if (e.kind != TRK_TRAP && e.kind != TRK_CALL) argstr += QString::asprintf("  0x%llx", (unsigned long long)e.rv);
    auto* ar = new QTableWidgetItem(argstr); ar->setFont(mono);
    ar->setForeground(e.kind == TRK_TRAP ? QColor("#cd5c5c") : QColor("#b0b6c0"));
    table->setItem(row, 8, ar);
}

void TracePage::rebuild_from_backing()
{
    table->setRowCount(0);
    shown_events = 0;
    // insert newest first (like live append)
    for (int i = backing.size() - 1; i >= 0; i--) {
        if (!filter_match(backing[i])) continue;
        insert_row(table->rowCount(), backing[i], i);
        shown_events++;
    }
    refresh_stats();
}

void TracePage::render_details(const TraceEvent& e)
{
    QString out;
    QString irqlName = e.irql == 0 ? "PASSIVE_LEVEL" : (e.irql == 1 ? "APC_LEVEL" : (e.irql == 2 ? "DISPATCH_LEVEL" : QString("IRQL %1").arg(e.irql)));
    out += QString("<pre style='font-family:JetBrainsMono NF, Consolas; font-size:12px; color:#d4d4d4;'>");
    out += QString("<b style='color:%1;'>%2</b>  <span style='color:#8a919e;'>at %3 | module %4 | TID %5 | %6</span>\n")
        .arg(trace_kind_col(e.kind).name(), trace_kind_str(e.kind), e.ts).arg(e.module_id).arg(e.thread_id).arg(irqlName);
    out += QString("<b>function</b>   %1\n").arg(e.name.toHtmlEscaped());

    const ArgSpec* spec = find_argspec(e.name);
    if (!e.argv.isEmpty()) {
        out += "<b>arguments</b>\n";
        for (int i = 0; i < e.argv.size(); i++) {
            QString name = (spec && spec->names[i][0]) ? spec->names[i] : QString("arg%1").arg(i);
            char type = (spec && spec->types[i]) ? spec->types[i] : 'X';
            out += QString("  <span style='color:#9ca3af;'>%1</span>  %2\n")
                .arg(name.leftJustified(12, ' ')).arg(fmt_arg_val(type, e.argv[i]));
        }
    }
    if (e.kind == TRK_TRAP) {
        out += QString("<b style='color:#cd5c5c;'>trap reason</b>  %1\n").arg(e.name.toHtmlEscaped());
    } else if (e.kind != TRK_CALL) {
        out += QString("<b>return</b>     0x%1  (%2)\n")
            .arg((unsigned long long)e.rv, 0, 16).arg((unsigned long long)e.rv);
    }
    out += "</pre>";
    details->setPlainText("");
    details->appendHtml(out);
}

void TracePage::refresh_stats()
{
    QString s;
    s = QString("events: %1 total | %2 shown").arg(total_events).arg(shown_events);
    if (!kind_counts.isEmpty()) {
        s += " | ";
        QStringList parts;
        for (auto it = kind_counts.constBegin(); it != kind_counts.constEnd(); ++it)
            parts << QString("%1=%2").arg(it.key()).arg(it.value());
        s += parts.join(" ");
    }
    statsLbl->setText(s);
}

void TracePage::append(const QVector<TraceEvent>& events)
{
    if (paused) return;
    for (const auto& e : events) {
        if (first_ts == 0) first_ts = e.timestamp_100ns;
        int back_idx = backing.size();
        backing.push_back(e);
        total_events++;
        kind_counts[trace_kind_str(e.kind)]++;

        if (!filter_match(e)) continue;
        insert_row(0, e, back_idx);
        shown_events++;
        if (table->rowCount() > 5000) table->removeRow(5000);
    }
    while (backing.size() > 20000) {
        backing.pop_front();
        bookmarks.clear();  // indices invalidated
    }
    refresh_stats();
}

void TracePage::saveTraceFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) { emit toast("cannot open file", 1); return; }
    if (path.endsWith(".csv", Qt::CaseInsensitive)) {
        QTextStream out(&f);
        out << "time,delta_us,tid,irql,module,kind,function,args,result\n";
        for (const auto& e : backing) {
            quint64 dt = (first_ts && e.timestamp_100ns >= first_ts) ? (e.timestamp_100ns - first_ts) / 10 : 0;
            out << e.ts << "," << dt << "," << e.thread_id << "," << e.irql << ","
                << e.module_id << "," << trace_kind_str(e.kind) << "," << e.name << ",\""
                << short_args_display(e) << "\","
                << QString::asprintf("0x%llx", (unsigned long long)e.rv) << "\n";
        }
    } else {
        // .gtrace binary: [magic 'GTR1'][count u32][first_ts u64][entries...]
        QDataStream ds(&f);
        ds.setByteOrder(QDataStream::LittleEndian);
        ds << (quint32)0x31525447u << (quint32)backing.size() << (quint64)first_ts;
        for (const auto& e : backing) {
            ds << e.timestamp_100ns << e.module_id << e.kind << e.thread_id << e.irql
               << (quint32)e.argv.size() << e.rv;
            ds << e.name;
            for (int i = 0; i < 4; i++) ds << (quint64)(i < e.argv.size() ? e.argv[i] : 0);
        }
    }
    f.close();
    emit toast(QString("saved %1 events -> %2").arg(backing.size()).arg(QFileInfo(path).fileName()), 2);
}

void TracePage::loadTraceFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { emit toast("cannot open file", 1); return; }
    QDataStream ds(&f);
    ds.setByteOrder(QDataStream::LittleEndian);
    quint32 magic = 0, count = 0;
    quint64 fts = 0;
    ds >> magic >> count >> fts;
    if (magic != 0x31525447u) { emit toast("not a .gtrace file", 1); return; }
    clear();
    first_ts = fts;
    QVector<TraceEvent> loaded;
    for (quint32 i = 0; i < count; i++) {
        TraceEvent e;
        quint32 argc;
        ds >> e.timestamp_100ns >> e.module_id >> e.kind >> e.thread_id >> e.irql >> argc >> e.rv;
        ds >> e.name;
        for (int a = 0; a < 4; a++) {
            quint64 v; ds >> v; if ((quint32)a < argc) e.argv.push_back(v);
        }
        SYSTEMTIME sys; FILETIME ft, lft;
        ft.dwLowDateTime  = (DWORD) e.timestamp_100ns;
        ft.dwHighDateTime = (DWORD)(e.timestamp_100ns >> 32);
        FileTimeToLocalFileTime(&ft, &lft); FileTimeToSystemTime(&lft, &sys);
        e.ts = QString::asprintf("%02u:%02u:%02u.%03u", sys.wHour, sys.wMinute, sys.wSecond, sys.wMilliseconds);
        loaded.push_back(e);
    }
    f.close();
    append(loaded);
    emit toast(QString("loaded %1 events from %2").arg(count).arg(QFileInfo(path).fileName()), 2);
}

void TracePage::clear()
{
    table->setRowCount(0); backing.clear(); kind_counts.clear();
    total_events = 0; shown_events = 0; details->clear(); refresh_stats();
}

// ExplorerPage

ExplorerPage::ExplorerPage(DriverClient* d, QWidget* p) : QWidget(p), drv(d)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 8);
    root->setSpacing(12);

    auto* topbar = new QFrame;
    topbar->setObjectName("toolbar");
    auto* tb = new QHBoxLayout(topbar);
    tb->setContentsMargins(14, 10, 14, 10);
    tb->setSpacing(10);
    auto* explain = new QLabel("Explorer uses a helper guest (toolkit.wasm) to read kernel state.");
    explain->setStyleSheet("color:#999999; font-size:12px;");
    tb->addWidget(explain);
    tb->addStretch();
    toolkitLbl = new QLabel("toolkit: not loaded");
    toolkitLbl->setStyleSheet("color:#cd5c5c; font-weight:600;");
    tb->addWidget(toolkitLbl);
    loadToolkitBtn = new QPushButton("Load toolkit");
    loadToolkitBtn->setObjectName("btnPrimary");
    loadToolkitBtn->setToolTip("Loads toolkit.wasm from the app directory (or pick another).");
    tb->addWidget(loadToolkitBtn);
    root->addWidget(topbar);

    auto* tabs = new QTabWidget;
    tabs->setObjectName("wbTabs");

    // processes tab
    {
        auto* w = new QWidget;
        auto* l = new QVBoxLayout(w);
        l->setContentsMargins(0, 8, 0, 0);
        auto* row = new QHBoxLayout;
        auto* refreshBtn = new QPushButton("Refresh");
        refreshBtn->setObjectName("btnPrimary");
        row->addWidget(refreshBtn); row->addStretch();
        l->addLayout(row);
        procTable = new QTableWidget(0, 4);
        procTable->setObjectName("dataTable");
        procTable->setHorizontalHeaderLabels({"PID", "Name", "EPROCESS", "Threads"});
        procTable->horizontalHeader()->setStretchLastSection(false);
        procTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        procTable->setColumnWidth(0, 80);
        procTable->setColumnWidth(2, 200);
        procTable->setColumnWidth(3, 80);
        procTable->verticalHeader()->setVisible(false);
        procTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
        procTable->setShowGrid(false);
        procTable->setAlternatingRowColors(true);
        l->addWidget(procTable, 1);
        connect(refreshBtn, &QPushButton::clicked, this, &ExplorerPage::refresh_procs);
        tabs->addTab(w, "Processes");
    }

    // memory tab
    {
        auto* w = new QWidget;
        auto* l = new QVBoxLayout(w);
        l->setContentsMargins(0, 8, 0, 0);
        auto* row = new QHBoxLayout;
        row->addWidget(new QLabel("Address:"));
        memAddr = new QLineEdit("0xfffff800`00000000");
        memAddr->setFont(QFont("JetBrainsMono NF", 10));
        memAddr->setFixedWidth(280);
        row->addWidget(memAddr);
        row->addWidget(new QLabel("Bytes:"));
        memLen = new QSpinBox; memLen->setRange(16, 4096); memLen->setValue(256);
        memLen->setFixedWidth(80);
        row->addWidget(memLen);
        auto* readBtn = new QPushButton("Read");
        readBtn->setObjectName("btnPrimary");
        row->addWidget(readBtn);
        row->addStretch();
        l->addLayout(row);
        memHex = new QPlainTextEdit;
        memHex->setReadOnly(true);
        memHex->setFont(QFont("JetBrainsMono NF", 10));
        memHex->setObjectName("hexView");
        l->addWidget(memHex, 1);
        connect(readBtn, &QPushButton::clicked, this, &ExplorerPage::read_memory);
        tabs->addTab(w, "Memory");
    }

    // MSR tab
    {
        auto* w = new QWidget;
        auto* l = new QVBoxLayout(w);
        l->setContentsMargins(0, 8, 0, 0);
        auto* row = new QHBoxLayout;
        auto* readBtn = new QPushButton("Read presets");
        readBtn->setObjectName("btnPrimary");
        row->addWidget(readBtn);
        msrCustom = new QLineEdit;
        msrCustom->setPlaceholderText("custom MSR index (hex or dec)");
        msrCustom->setFont(QFont("JetBrainsMono NF", 10));
        row->addWidget(msrCustom);
        auto* customBtn = new QPushButton("Read");
        row->addWidget(customBtn);
        row->addStretch();
        l->addLayout(row);
        msrTable = new QTableWidget(0, 3);
        msrTable->setObjectName("dataTable");
        msrTable->setHorizontalHeaderLabels({"MSR", "Name", "Value"});
        msrTable->horizontalHeader()->setStretchLastSection(false);
        msrTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        msrTable->setColumnWidth(0, 100);
        msrTable->setColumnWidth(2, 220);
        msrTable->verticalHeader()->setVisible(false);
        msrTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
        msrTable->setShowGrid(false);
        msrTable->setAlternatingRowColors(true);
        l->addWidget(msrTable, 1);
        connect(readBtn, &QPushButton::clicked, this, &ExplorerPage::read_msrs);
        connect(customBtn, &QPushButton::clicked, this, [this]{
            if (toolkit_id < 0) { emit toast("load toolkit first", 1); return; }
            QString s = msrCustom->text().trimmed(); if (s.isEmpty()) return;
            bool ok = false;
            quint32 idx = s.startsWith("0x", Qt::CaseInsensitive) ? s.mid(2).toUInt(&ok, 16) : s.toUInt(&ok, 0);
            if (!ok) { emit toast("bad MSR", 1); return; }
            QVector<quint64> argv = { idx };
            quint64 rv = 0; QString err;
            if (drv->call_export(toolkit_id, "toolkit_readmsr", 2000, argv, rv, err)) {
                int row = msrTable->rowCount();
                msrTable->insertRow(row);
                msrTable->setItem(row, 0, new QTableWidgetItem(QString::asprintf("0x%x", idx)));
                msrTable->setItem(row, 1, new QTableWidgetItem("(custom)"));
                msrTable->setItem(row, 2, new QTableWidgetItem(QString::asprintf("0x%016llx", (unsigned long long)rv)));
                for (int c = 0; c < 3; c++) msrTable->item(row, c)->setFont(QFont("JetBrainsMono NF", 10));
            } else emit toast(err, 1);
        });
        tabs->addTab(w, "MSRs");
    }

    // CPUID tab
    {
        auto* w = new QWidget;
        auto* l = new QVBoxLayout(w);
        l->setContentsMargins(0, 8, 0, 0);
        auto* row = new QHBoxLayout;
        row->addWidget(new QLabel("Max leaf:"));
        cpuidMax = new QSpinBox; cpuidMax->setRange(0, 0x40); cpuidMax->setValue(0x18);
        cpuidMax->setFixedWidth(80);
        row->addWidget(cpuidMax);
        auto* dumpBtn = new QPushButton("Dump");
        dumpBtn->setObjectName("btnPrimary");
        row->addWidget(dumpBtn);
        row->addStretch();
        l->addLayout(row);
        cpuidTable = new QTableWidget(0, 6);
        cpuidTable->setObjectName("dataTable");
        cpuidTable->setHorizontalHeaderLabels({"Leaf", "Sub", "EAX", "EBX", "ECX", "EDX"});
        cpuidTable->horizontalHeader()->setStretchLastSection(true);
        cpuidTable->setColumnWidth(0, 70);
        cpuidTable->setColumnWidth(1, 50);
        for (int i = 2; i <= 5; i++) cpuidTable->setColumnWidth(i, 110);
        cpuidTable->verticalHeader()->setVisible(false);
        cpuidTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
        cpuidTable->setShowGrid(false);
        cpuidTable->setAlternatingRowColors(true);
        l->addWidget(cpuidTable, 1);
        connect(dumpBtn, &QPushButton::clicked, this, &ExplorerPage::read_cpuid);
        tabs->addTab(w, "CPUID");
    }

    root->addWidget(tabs, 1);

    connect(loadToolkitBtn, &QPushButton::clicked, this, [this]{
        QString here = QCoreApplication::applicationDirPath();
        QString def  = here + "/toolkit.wasm";
        QString path;
        if (QFile::exists(def)) {
            // one-click if the shipped toolkit is next to the exe
            path = def;
        } else {
            path = QFileDialog::getOpenFileName(this, "Select toolkit.wasm",
                QString(), "WebAssembly (*.wasm)");
            if (path.isEmpty()) return;
        }
        quint32 id = 0; QString err;
        if (drv->load_module(path, 0, id, err)) {
            setToolkitId((int)id);
            emit toast(QString("toolkit loaded as module %1").arg(id), 2);
        } else emit toast("load failed: " + err, 1);
    });
}

void ExplorerPage::setToolkitId(int id)
{
    toolkit_id = id;
    if (id < 0) {
        toolkitLbl->setText("toolkit: not loaded");
        toolkitLbl->setStyleSheet("color:#cd5c5c; font-weight:600;");
    } else {
        toolkitLbl->setText(QString("toolkit: module %1").arg(id));
        toolkitLbl->setStyleSheet("color:#8fbf5c; font-weight:600;");
    }
}

void ExplorerPage::refresh_procs()
{
    if (toolkit_id < 0) { emit toast("load toolkit first", 1); return; }
    // 1. get scratch offset
    QVector<quint64> a0; quint64 scratch = 0; QString err;
    if (!drv->call_export(toolkit_id, "toolkit_scratch_off", 2000, a0, scratch, err))
        { emit toast("toolkit_scratch_off: " + err, 1); return; }
    // 2. enumerate procs into scratch
    QVector<quint64> a1 = { scratch, 64 };
    quint64 count = 0;
    if (!drv->call_export(toolkit_id, "toolkit_enum_procs", 5000, a1, count, err))
        { emit toast("toolkit_enum_procs: " + err, 1); return; }
    // 3. read scratch back
    QByteArray data;
    if (!drv->read_guest((quint32)toolkit_id, (quint32)scratch, (quint32)(count * 32), data, err))
        { emit toast("read_guest: " + err, 1); return; }
    procTable->setRowCount((int)count);
    QFont mono("JetBrainsMono NF", 10);
    for (int i = 0; i < (int)count; i++) {
        const char* p = data.constData() + i * 32;
        quint64 pid    = *(const quint64*)(p +  0);
        quint64 eproc  = *(const quint64*)(p +  8);
        char name[16] = {0};
        memcpy(name, p + 16, 15);
        auto* c0 = new QTableWidgetItem(QString::number(pid)); c0->setFont(mono);
        auto* c1 = new QTableWidgetItem(QString::fromLatin1(name));
        auto* c2 = new QTableWidgetItem(QString::asprintf("0x%016llx", (unsigned long long)eproc));
        c2->setFont(mono); c2->setForeground(QColor("#8a8a8a"));
        auto* c3 = new QTableWidgetItem("-"); c3->setFont(mono);
        procTable->setItem(i, 0, c0); procTable->setItem(i, 1, c1);
        procTable->setItem(i, 2, c2); procTable->setItem(i, 3, c3);
    }
}

void ExplorerPage::read_memory()
{
    if (toolkit_id < 0) { emit toast("load toolkit first", 1); return; }
    QString a = memAddr->text(); a.remove('`').remove(' ');
    bool ok = false;
    quint64 va = a.startsWith("0x", Qt::CaseInsensitive) ? a.mid(2).toULongLong(&ok, 16) : a.toULongLong(&ok, 0);
    if (!ok) { emit toast("bad address", 1); return; }
    QString err;
    quint64 scratch = 0;
    if (!drv->call_export(toolkit_id, "toolkit_scratch_off", 2000, {}, scratch, err))
        { emit toast(err, 1); return; }
    quint32 len = (quint32)memLen->value();
    QVector<quint64> argv = { va, scratch, len };
    quint64 bytes_read = 0;
    if (!drv->call_export(toolkit_id, "toolkit_read_kmem", 2000, argv, bytes_read, err))
        { emit toast(err, 1); return; }
    if (bytes_read == 0) { memHex->setPlainText("(zero bytes read - probably invalid address)"); return; }
    QByteArray data;
    if (!drv->read_guest((quint32)toolkit_id, (quint32)scratch, (quint32)bytes_read, data, err))
        { emit toast("read_guest: " + err, 1); return; }
    QString out;
    for (int off = 0; off < data.size(); off += 16) {
        out += QString::asprintf("%016llx  ", (unsigned long long)(va + off));
        QString hex, ascii;
        for (int i = 0; i < 16; i++) {
            if (off + i < data.size()) {
                unsigned char b = (unsigned char)data[off + i];
                hex += QString::asprintf("%02x ", b);
                ascii += (b >= 0x20 && b < 0x7f) ? QChar(b) : '.';
            } else { hex += "   "; ascii += ' '; }
            if (i == 7) hex += " ";
        }
        out += hex + " " + ascii + "\n";
    }
    memHex->setPlainText(out);
}

static const struct { quint32 idx; const char* name; } MSR_PRESETS[] = {
    { 0x001b, "IA32_APIC_BASE" },
    { 0x003a, "IA32_FEATURE_CONTROL" },
    { 0x00c0, "IA32_PMC0" },
    { 0x00c1, "IA32_PMC1" },
    { 0x0174, "IA32_SYSENTER_CS" },
    { 0x0175, "IA32_SYSENTER_ESP" },
    { 0x0176, "IA32_SYSENTER_EIP" },
    { 0x01d9, "IA32_DEBUGCTL" },
    { 0x0277, "IA32_PAT" },
    { 0x02ff, "IA32_MTRR_DEF_TYPE" },
    { 0xc0000080, "IA32_EFER" },
    { 0xc0000081, "IA32_STAR" },
    { 0xc0000082, "IA32_LSTAR" },
    { 0xc0000083, "IA32_CSTAR" },
    { 0xc0000084, "IA32_FMASK" },
    { 0xc0000100, "IA32_FS_BASE" },
    { 0xc0000101, "IA32_GS_BASE" },
    { 0xc0000102, "IA32_KERNEL_GS_BASE" },
    { 0xc0000103, "IA32_TSC_AUX" },
    { 0, nullptr }
};

void ExplorerPage::read_msrs()
{
    if (toolkit_id < 0) { emit toast("load toolkit first", 1); return; }
    msrTable->setRowCount(0);
    QFont mono("JetBrainsMono NF", 10);
    for (int i = 0; MSR_PRESETS[i].name; i++) {
        QVector<quint64> argv = { MSR_PRESETS[i].idx };
        quint64 rv = 0; QString err;
        bool ok = drv->call_export(toolkit_id, "toolkit_readmsr", 2000, argv, rv, err);
        int row = msrTable->rowCount();
        msrTable->insertRow(row);
        msrTable->setItem(row, 0, new QTableWidgetItem(QString::asprintf("0x%x", MSR_PRESETS[i].idx)));
        msrTable->setItem(row, 1, new QTableWidgetItem(MSR_PRESETS[i].name));
        msrTable->setItem(row, 2, new QTableWidgetItem(ok ? QString::asprintf("0x%016llx", (unsigned long long)rv) : QString("err: " + err)));
        for (int c = 0; c < 3; c++) msrTable->item(row, c)->setFont(mono);
        if (!ok) msrTable->item(row, 2)->setForeground(QColor("#cd5c5c"));
    }
}

void ExplorerPage::read_cpuid()
{
    if (toolkit_id < 0) { emit toast("load toolkit first", 1); return; }
    cpuidTable->setRowCount(0);
    QFont mono("JetBrainsMono NF", 10);
    QString err;
    quint64 scratch = 0;
    if (!drv->call_export(toolkit_id, "toolkit_scratch_off", 2000, {}, scratch, err))
        { emit toast(err, 1); return; }
    int maxL = cpuidMax->value();
    for (int leaf = 0; leaf <= maxL; leaf++) {
        QVector<quint64> argv = { (quint32)leaf, 0, (quint32)scratch };
        quint64 rv = 0;
        if (!drv->call_export(toolkit_id, "toolkit_cpuid", 2000, argv, rv, err)) continue;
        QByteArray data;
        if (!drv->read_guest((quint32)toolkit_id, (quint32)scratch, 16, data, err)) continue;
        quint32 eax = *(const quint32*)(data.constData() + 0);
        quint32 ebx = *(const quint32*)(data.constData() + 4);
        quint32 ecx = *(const quint32*)(data.constData() + 8);
        quint32 edx = *(const quint32*)(data.constData() + 12);
        int row = cpuidTable->rowCount();
        cpuidTable->insertRow(row);
        cpuidTable->setItem(row, 0, new QTableWidgetItem(QString::asprintf("0x%02x", leaf)));
        cpuidTable->setItem(row, 1, new QTableWidgetItem("0"));
        cpuidTable->setItem(row, 2, new QTableWidgetItem(QString::asprintf("0x%08x", eax)));
        cpuidTable->setItem(row, 3, new QTableWidgetItem(QString::asprintf("0x%08x", ebx)));
        cpuidTable->setItem(row, 4, new QTableWidgetItem(QString::asprintf("0x%08x", ecx)));
        cpuidTable->setItem(row, 5, new QTableWidgetItem(QString::asprintf("0x%08x", edx)));
        for (int c = 0; c < 6; c++) cpuidTable->item(row, c)->setFont(mono);
    }
}
