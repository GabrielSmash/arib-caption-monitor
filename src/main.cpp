#include "MainWindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QMessageBox>
#include <QPixmap>
#include <QTimer>

int main(int argc, char* argv[]) {
    // Qt 6.10's PipeWire audio backend does not list Bluetooth outputs and reports the wrong default device;
    // the PulseAudio backend (served by pipewire-pulse) follows the system default correctly.
    if (qEnvironmentVariableIsEmpty("QT_AUDIO_BACKEND"))
        qputenv("QT_AUDIO_BACKEND", "pulseaudio");

    QApplication app(argc, argv);
    QApplication::setOrganizationName("arib-caption-monitor");
    QApplication::setApplicationName("arib-caption-monitor");

    QCommandLineParser parser;
    parser.setApplicationDescription("Monitor de legendas ARIB (ABNT NBR 15606-1) recebidas por MPEG-TS/UDP");
    parser.addHelpOption();
    QCommandLineOption portOption({"p", "port"}, "Porta UDP de recepção.", "porta");
    QCommandLineOption groupOption({"m", "multicast"}, "Grupo multicast (opcional).", "endereço");
    QCommandLineOption snapshotOption("snapshot", "Salva uma captura da janela e encerra (teste).", "arquivo.png");
    QCommandLineOption snapshotAfterOption("snapshot-after", "Segundos até a captura (padrão 10).", "segundos", "10");
    QCommandLineOption fileOption({"f", "file"}, "Reproduz um arquivo MPEG-TS em vez de escutar UDP.", "arquivo.ts");
    QCommandLineOption tabOption("tab", "Aba do painel ao abrir (0 MPEG-TS, 1 Validação, 2 Comandos, 3 Log).", "índice");
    parser.addOptions({portOption, groupOption, snapshotOption, snapshotAfterOption, tabOption, fileOption});
    parser.process(app);

    MainWindow window;
    QString error;
    if (!window.initialize(&error)) {
        QMessageBox::critical(nullptr, "ARIB Caption Monitor", error);
        return 1;
    }
    if (parser.isSet(portOption))
        window.setEndpoint(static_cast<quint16>(parser.value(portOption).toUInt()), parser.value(groupOption));
    if (parser.isSet(tabOption))
        window.showAnalysisTab(parser.value(tabOption).toInt());
    window.show();
    if (parser.isSet(fileOption))
        window.openFile(parser.value(fileOption));
    else
        window.startReceiving();

    if (parser.isSet(snapshotOption)) {
        const QString path = parser.value(snapshotOption);
        QTimer::singleShot(parser.value(snapshotAfterOption).toInt() * 1000, &window, [&window, path]() {
            const QPixmap shot = window.grab();
            if (!shot.save(path))
                qWarning("snapshot: falha ao salvar %s (%dx%d)", qPrintable(path), shot.width(), shot.height());
            QApplication::quit();
        });
    }

    return app.exec();
}
