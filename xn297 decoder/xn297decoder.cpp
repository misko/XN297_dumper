﻿#include "xn297decoder.h"
//#include <QThread>
#include <QFile>
#include <QFileDialog>
#include <QPixmap>
#include <QIcon>

xn297decoder::xn297decoder(QWidget *parent)
    : QMainWindow(parent)
{ 
    ui.setupUi(this);
    setWindowIcon(QIcon(QPixmap(":/xn297decoder/Resources/icon.png")));
    setFixedSize(this->geometry().width(),this->geometry().height());
    setWindowFlags(Qt::MSWindowsFixedSizeDialogHint);
    statusBar()->setSizeGripEnabled(false);
    label_statusPps = new QLabel(this);
    label_statusPps->setFrameStyle(QFrame::Panel | QFrame::Sunken);
    label_statusHearthbeat = new QLabel(this);
    label_statusHearthbeat->setFrameStyle(QFrame::Panel | QFrame::Sunken);
    label_statusSeparator = new QLabel(this);
    label_statusSeparator->setFixedWidth(2);
    ui.statusBar->addWidget(label_statusSeparator);
    ui.statusBar->addWidget(label_statusHearthbeat);
    ui.statusBar->addWidget(label_statusPps);

    rpc = new MaiaXmlRpcClient(QUrl("http://localhost:1235"), this);

    connect(ui.spinBox_channel, SIGNAL(valueChanged(int)), this, SLOT(spinBox_channelChanged(int)));
    connect(ui.spinBox_fineTune, SIGNAL(valueChanged(int)), this, SLOT(spinBox_fineTuneChanged(int)));
    connect(ui.spinBox_addressLength, SIGNAL(valueChanged(int)), this, SLOT(spinBox_addressLengthChanged(int)));
    connect(ui.spinBox_payloadLength, SIGNAL(valueChanged(int)), this, SLOT(spinBox_payloadLengthChanged(int)));
    connect(ui.radioButton_bitrate1M, SIGNAL(toggled(bool)), this, SLOT(radioButton_bitrate1MChanged()));
    connect(ui.pushButton_locateGnuradio, SIGNAL(clicked()), this, SLOT(pushButton_locateGnuradioClicked()));
    connect(ui.pushButton_startStopFlowgraph, SIGNAL(clicked()), this, SLOT(pushButton_startStopFlowgraphClicked()));

    settings = new QSettings(QSettings::IniFormat, QSettings::UserScope, "Goebish Apps", "xn297 decoder", this);
    load_settings();

    pps_timer = new QTimer(this);
    pps_counter = 0;
    connect(pps_timer, SIGNAL(timeout()), this, SLOT(show_pps()));
    pps_timer->start(1000);

    rpc_hearthbeat_timer = new QTimer(this);
    is_rpc_connected = false;
    connect(rpc_hearthbeat_timer, SIGNAL(timeout()), this, SLOT(rpc_hearthbeat()));
    rpc_hearthbeat();
    rpc_hearthbeat_timer->start(3000);

    socket = new QUdpSocket(this);
    socket->bind(QHostAddress::LocalHost, 1234);
    connect(socket, SIGNAL(readyRead()), this, SLOT(readPendingDatagrams()));

    gnuradio_process = new QProcess(this);
    connect(gnuradio_process, SIGNAL(stateChanged(QProcess::ProcessState)), this, SLOT(gnuradio_processStateChanged(QProcess::ProcessState)));
    connect(gnuradio_process, SIGNAL(readyReadStandardOutput()), this, SLOT(gnuradio_processStdOutput()));
    connect(gnuradio_process, SIGNAL(readyReadStandardError()), this, SLOT(gnuradio_processStdError()));
}

void xn297decoder::load_settings()
{
    bool ok;
    ui.spinBox_channel->setValue(settings->value("channel","0").toInt(&ok));
    rpc_set("channel", settings->value("channel","0").toInt(&ok));
    ui.spinBox_fineTune->setValue(settings->value("finetune","100").toInt(&ok));
    rpc_set("freq_fine", settings->value("finetune","100").toInt(&ok)*1000);
    addressLength = settings->value("address_length","0").toInt(&ok);
    ui.spinBox_addressLength->setValue(addressLength);
    payloadLength = settings->value("payload_length","0").toInt(&ok);
    ui.spinBox_payloadLength->setValue(payloadLength);
    if(settings->value("bitrate","1M") == "1M")
        ui.radioButton_bitrate1M->setChecked(true);
    else
        ui.radioButton_bitrate250k->setChecked(true);
    radioButton_bitrate1MChanged();
    if(QFile::exists(settings->value("gnuradio_launcher","").toString()))
        ui.pushButton_startStopFlowgraph->setEnabled(true);
}

void xn297decoder::run_gr_flowgraph()
{
    if(!QFile::exists(settings->value("gnuradio_launcher","").toString())) {
        ui.plainTextEdit->appendPlainText("gnuradio launcher not found");
        return;
    }
    if(!QFile::exists(GR_FLOWGRAPH)) {
        ui.plainTextEdit->appendPlainText("gnuradio flow graph " + (QString)GR_FLOWGRAPH + " not found");
        return;
    }    
    ui.plainTextEdit->appendPlainText("launching gnuradio flow graph");
    ui.plainTextEdit->appendPlainText("> " + settings->value("gnuradio_launcher","").toString() + " -u " + (QString)GR_FLOWGRAPH);
    gnuradio_process->start(settings->value("gnuradio_launcher","").toString(), QStringList() << "-u" << GR_FLOWGRAPH);
}

uint8_t xn297decoder::bit_reverse(uint8_t b_in)
{
    uint8_t b_out = 0;
    for (uint8_t i = 0; i < 8; ++i) {
        b_out = (b_out << 1) | (b_in & 1);
        b_in >>= 1;
    }
    return b_out;
}

uint16_t xn297decoder::crc16_update(uint16_t crc, uint8_t a, uint8_t bits)
{
    const uint16_t polynomial = 0x1021;
    crc ^= a << 8;
    while (bits--) {
        if (crc & 0x8000) {
            crc = (crc << 1) ^ polynomial;
        } else {
            crc = crc << 1;
        }
    }
    return crc;
}

void xn297decoder::readPendingDatagrams()
{
    // xn297 scramble table
    const uint8_t xn297_scramble[] = {
    0xe3, 0xb1, 0x4b, 0xea, 0x85, 0xbc, 0xe5, 0x66,
    0x0d, 0xae, 0x8c, 0x88, 0x12, 0x69, 0xee, 0x1f,
    0xc7, 0x62, 0x97, 0xd5, 0x0b, 0x79, 0xca, 0xcc,
    0x1b, 0x5d, 0x19, 0x10, 0x24, 0xd3, 0xdc, 0x3f,
    0x8e, 0xc5, 0x2f};

    const uint16_t xn297_crc_xorout_scrambled[] = {
    0x0000, 0x3448, 0x9BA7, 0x8BBB, 0x85E1, 0x3E8C,
    0x451E, 0x18E6, 0x6B24, 0xE7AB, 0x3828, 0x814B,
    0xD461, 0xF494, 0x2503, 0x691D, 0xFE8B, 0x9BA7,
    0x8B17, 0x2920, 0x8B5F, 0x61B1, 0xD391, 0x7401,
    0x2138, 0x129F, 0xB3A0, 0x2988};

    const uint16_t crc_initial = 0xb5d2;
    const uint8_t crc_size = 2;

    static uint8_t byte, bit_count, byte_count, crc_index;
	static bool in_packet = false;
    static uint16_t crc, packet_crc;
    static QString log;
    QString temp;

    while(socket->hasPendingDatagrams()) {
        QNetworkDatagram datagram = socket->receiveDatagram();
        for(uint i=0; i<datagram.data().size(); i++) {
            uint8_t bit = (uint8_t)datagram.data().at(i);
                if((bit & 0x02)) { // found correlate access code bit (1st bit of address)
                byte = 0;
			    bit_count = 0;
			    byte_count = 0;
                crc = crc_initial;
                crc_index = 1;
                packet_crc = 0;
                log.clear();
			    in_packet = true;
		    }
        
		    if(in_packet) {
			    if(bit & 0x01) {
				    byte |= 1 << (7-bit_count);
			    }
			    bit_count++;
			    if(bit_count > 7) {
				    if(byte_count < addressLength) {
                        crc = crc16_update(crc, byte, 8);
					    byte = byte ^ xn297_scramble[byte_count];
                    }
				    else if(byte_count < addressLength + payloadLength) {
                        crc = crc16_update(crc, byte, 8);
					    byte = bit_reverse(byte ^ xn297_scramble[byte_count]);
				    }
                    if(byte_count == addressLength || byte_count == addressLength + payloadLength)
                        log += "<b>|</b> ";
				    if(byte_count < addressLength + payloadLength)
                        log += temp.sprintf("%02X ", byte);
                    else // crc bytes
                        packet_crc |= (uint16_t)byte << (8*crc_index--);
                    bit_count = 0;
				    byte = 0;
				    byte_count++;
				    if(byte_count == addressLength + payloadLength + crc_size) {
					    in_packet = false;
                        crc ^= xn297_crc_xorout_scrambled[addressLength-3+payloadLength];
                        if(packet_crc == crc) 
                            log += temp.sprintf("<font color='green'>%02X %02X", packet_crc>>8, packet_crc&0xff);
                        else
                            log += temp.sprintf("<font color='red'><b>%02X %02X", packet_crc>>8, packet_crc&0xff);
                        ui.plainTextEdit->appendHtml(log);
                        pps_counter++;
				    }
			    }
            }
		}
    }
}

void xn297decoder::rpc_set(const QString & key, int value)
{
    rpc->call("set_" + key, QVariantList() << value, 
              this, SLOT(rpc_response(QVariant &)),
              this, SLOT(rpc_fault(int, const QString &)));
}

void xn297decoder::rpc_response(QVariant &response)
{
    
}

void xn297decoder::rpc_fault(int, const QString &fault)
{
    
}

void xn297decoder::rpc_hearthbeat_response(QVariant &response)
{
    if(!is_rpc_connected) {
        label_statusHearthbeat->setText("RPC hearthbeat Ok");
        is_rpc_connected = true;
        // problem: we got the response before the flow graph is totally constructed
        // wait a while before sending settings ...
        // seems that's only required when the flow graph is started from gnuradio companion
        //QThread::msleep(1000);
        load_settings();
    }
}

void xn297decoder::rpc_hearthbeat_fault(int, const QString &fault)
{
    is_rpc_connected = false;
}

void xn297decoder::rpc_hearthbeat()
{
    rpc->call("set_hearthbeat", QVariantList() << 1, 
              this, SLOT(rpc_hearthbeat_response(QVariant &)),
              this, SLOT(rpc_hearthbeat_fault(int, const QString &)));
}

void xn297decoder::spinBox_channelChanged(int value)
{
    rpc_set("channel", value);
    uint freq = 2.4e9 + value*1e6 + ui.spinBox_fineTune->value()*1000; 
    ui.label_frequency->setText(QString::number((float)freq/1e6, 'f', 2) + " MHz");
    settings->setValue("channel", QString::number(value));
}

void xn297decoder::spinBox_fineTuneChanged(int value)
{
    rpc_set("freq_fine", value*1000);
    uint freq = 2.4e9 + ui.spinBox_channel->value()*1e6 + value*1000; 
    ui.label_frequency->setText(QString::number((float)freq/1e6, 'f', 2) + " MHz");
    settings->setValue("finetune", QString::number(value));
}

void xn297decoder::spinBox_addressLengthChanged(int value)
{
    addressLength = value;
    settings->setValue("address_length", QString::number(value));
}

void xn297decoder::spinBox_payloadLengthChanged(int value)
{
    payloadLength = value;
    settings->setValue("payload_length", QString::number(value));
}

void xn297decoder::show_pps()
{
    if(is_rpc_connected)
        label_statusPps->setText(QString::number(pps_counter) + " pps");
    else
        label_statusHearthbeat->setText("Waiting for RPC hearthbeat");
    pps_counter = 0;
}

void xn297decoder::radioButton_bitrate1MChanged()
{
    if(ui.radioButton_bitrate1M->isChecked()) {
        settings->setValue("bitrate", "1M");
        rpc_set("bitrate", 0);
    }
    else {
        settings->setValue("bitrate", "250k");
        rpc_set("bitrate", 1);
    }
}

void xn297decoder::pushButton_locateGnuradioClicked()
{
    QString file = QFileDialog::getOpenFileName(this, "Locate gnuradio launcher", settings->value("gnuradio_launcher", "C:\\Program files").toString() , "run_gr.bat", nullptr);
    if(QFile::exists(file)) {
        settings->setValue("gnuradio_launcher", file);
        ui.pushButton_startStopFlowgraph->setEnabled(true);
    }
}

void xn297decoder::pushButton_startStopFlowgraphClicked()
{
    if(gnuradio_process->state() == QProcess::ProcessState::Running) {
        gnuradio_process->kill();
    }
    else {
        run_gr_flowgraph();
    }
}

void xn297decoder::gnuradio_processStateChanged(QProcess::ProcessState newState) {
    switch(newState) {
        case QProcess::ProcessState::Running:
            ui.plainTextEdit->appendPlainText("flow graph starting");
            ui.pushButton_startStopFlowgraph->setText("stop gnuradio flow graph");
            break;
        case QProcess::ProcessState::NotRunning:
            ui.plainTextEdit->appendPlainText("gnuradio flow graph stopped");
            ui.pushButton_startStopFlowgraph->setText("start gnuradio flow graph");
            break;
    }
}

void xn297decoder::gnuradio_processStdOutput()
{
    QString output = (QString)(gnuradio_process->readAllStandardOutput());
    if(output.indexOf("Press Enter") < 0)
        ui.plainTextEdit->appendHtml(output);
}

void xn297decoder::gnuradio_processStdError()
{
    QString output = (QString)(gnuradio_process->readAllStandardError());
    // strip XMLRPC server debug log
    if(output.indexOf("POST") < 0)
        ui.plainTextEdit->appendHtml(output);
}
