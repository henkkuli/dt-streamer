const path = require('path');
const grpc = require('grpc');
const protoLoader = require('@grpc/proto-loader');
const express = require('express');
const http = require('http');
const socketIo = require('socket.io')
const grpc_promise = require('grpc-promise');
const fs = require('fs');

const OBS_USERNAMES = path.join(__dirname, 'obs');

// Load RPC definition
const PROTO_PATH = path.join(__dirname, '../control.proto');
const packageDefinition = protoLoader.loadSync(
    PROTO_PATH,
    {
        keepCase: true,
        defaults: true,
    }
);
const protoDescriptor = grpc.loadPackageDefinition(packageDefinition);
const Control = protoDescriptor.Control;

// Start web server
const app = express();
const server = http.Server(app);
const io = socketIo(server);
server.listen(8000);

// Connect RPC client
const client = new Control('localhost:6000', grpc.credentials.createInsecure());
grpc_promise.promisifyAll(client);

// Send index
app.get('/', function(req, res) {
    res.sendFile(path.join(__dirname, './index.html'));
});

function loadUserMapping() {
    const mapping = {};
    const raw_data = fs.readFileSync(path.join(__dirname, 'user-name-map.csv'), 'utf8');
    const lines = raw_data.split(/\n\r?/);
    for (let line of lines) {
        let [firstname, lastname, username] = line.split(',');
        mapping[username] = `${firstname} ${lastname}`;
    }
    return mapping;
}
let userMapping = loadUserMapping();

io.on('connection', function(socket) {
    async function updateAll() {
        try {
            let [{sources}, {sinks}] = await Promise.all([
                client.ListSources().sendMessage({}),
                client.listSinks().sendMessage({}),
            ]);
            socket.emit('update', {sources, sinks, userMapping});
        } catch (e) {
            socket.emit('log', e.toString());
        }
    }

    updateAll();

    socket.on('requestUpdate', updateAll);

    socket.on('route', async function({source, sink, username}) {
        console.log(arguments)
        try {
            await client.ConnectSourceToSink().sendMessage({source, sink});
            fs.writeFileSync(path.join(OBS_USERNAMES, `${sink}.txt`), username, 'utf8');
        } catch (e) {
            socket.emit('log', e.toString());
        }
    });
});
