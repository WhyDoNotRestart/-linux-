import Soup from 'gi://Soup';
import GLib from 'gi://GLib';

const loop = GLib.MainLoop.new(null, false);
const session = new Soup.Session({timeout: 3});
const msg = Soup.Message.new('GET', 'http://127.0.0.1:8787/api/v1/snapshot');

session.send_and_read_async(msg, GLib.PRIORITY_DEFAULT, null, (sess, res) => {
    try {
        const bytes = sess.send_and_read_finish(res);
        const text = new TextDecoder().decode(bytes.get_data());
        const d = JSON.parse(text);
        print('OK host=' + d.host.name + ' cpu=' + d.cpu.pct.toFixed(1) +
              ' mem=' + d.mem.total + ' procs=' + d.procs.length);
    } catch (e) {
        print('ERR ' + e);
    }
    loop.quit();
});
loop.run();
