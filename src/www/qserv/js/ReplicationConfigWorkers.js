define([
    'webfwk/CSSLoader',
    'webfwk/Fwk',
    'webfwk/FwkApplication',
    'qserv/Common',
    'underscore'],

function(CSSLoader,
         Fwk,
         FwkApplication,
         Common,
         _) {

    CSSLoader.load('qserv/css/ReplicationConfigWorkers.css');

    class ReplicationConfigWorkers extends FwkApplication {

        /**
         * @returns the default update interval for the page.
         */ 
        static update_ival_sec() { return 10; }

        constructor(name) {
            super(name);
        }

        /**
         * Override event handler defined in the base class.
         * @see FwkApplication.fwk_app_on_show
         */
        fwk_app_on_show() {
            this.fwk_app_on_update();
        }

        /**
         * Override event handler defined in the base class.
         * @see FwkApplication.fwk_app_on_hide
         */
        fwk_app_on_hide() {}

        /**
         * Override event handler defined in the base class.
         * @see FwkApplication.fwk_app_on_update
         */
        fwk_app_on_update() {
            if (this.fwk_app_visible) {
                if (this._prev_update_sec === undefined) {
                    this._prev_update_sec = 0;
                }
                let now_sec = Fwk.now().sec;
                if (now_sec - this._prev_update_sec > ReplicationConfigWorkers.update_ival_sec()) {
                    this._prev_update_sec = now_sec;
                    this._init();
                    this._load();
                }
            }
        }

        /**
         * The first time initialization of the page's layout.
         */
        _init() {
            if (this._initialized === undefined) {
                this._initialized = false;
            }
            if (this._initialized) return;
            this._initialized = true;

            let html = `
<div class="row">
  <div class="col">
    <table class="table table-sm table-hover" id="fwk-controller-config-workers">
      <thead class="thead-light">
        <tr>
          <th class="sticky">Worker</th>
          <th class="sticky">Enabled</th>
          <th class="sticky">Read-only</th>
          <th class="sticky">Service </th>
          <th class="sticky">Protocol </th>
          <th class="sticky">Port </th>
          <th class="sticky">IP</th>
          <th class="sticky">DNS</th>
        </tr>
      </thead>
      <caption class="updating">Loading...</caption>
      <tbody></tbody>
    </table>
  </div>
</div>`;
            this.fwk_app_container.html(html);
        }

        /**
         * Table for displaying Configuration parameters of the workers.
         * @returns JQuery table object
         */
        _table() {
            if (this._table_obj === undefined) {
                this._table_obj = this.fwk_app_container.find('table#fwk-controller-config-workers');
            }
            return this._table_obj;
        }

        /**
         * Load data from a web servie then render it to the application's page.
         */
        _load() {
            if (this._loading === undefined) {
                this._loading = false;
            }
            if (this._loading) return;
            this._loading = true;

            this._table().children('caption').addClass('updating');
            Fwk.web_service_GET(
                "/replication/config",
                {version: Common.RestAPIVersion},
                (data) => {
                    this._display(data);
                    Fwk.setLastUpdate(this._table().children('caption'));
                    this._table().children('caption').removeClass('updating');
                    this._loading = false;
                },
                (msg) => {
                    this._table().children('caption').html('<span style="color:maroon">No Response</span>');
                    this._table().children('caption').removeClass('updating');
                    this._table().children('caption').removeClass('updating');
                    this._loading = false;
                }
            );
        }

        /**
         * Display the configuration.
         */
        _display(data) {
            var config = data.config;
            let html = '';
            for (let i in config.workers) {
                let worker = config.workers[i];
                let workerEnabledCssClass  = worker['is-enabled']   ? '' : 'class="table-warning"';
                let workerReadOnlyCssClass = worker['is-read-only'] ? 'class="table-warning"' : '';
                const service = [
                    {   "name":     "Replica Management",
                        "protocol": "binary",
                        "port":      worker['svc-port'],
                        "ip":        worker['svc-host']['addr'],
                        "dns":       worker['svc-host']['name'],
                        "cssClass":  "bg-white"
                    },
                    {   "name":     "Replica Management (HTTP)",
                        "protocol": "http",
                        "port":      worker['http-svc-port'],
                        "ip":        worker['http-svc-host']['addr'],
                        "dns":       worker['http-svc-host']['name'],
                        "cssClass":  "bg-white"
                    },
                    {   "name":     "File Server",
                        "protocol": "binary",
                        "port":     worker['fs-port'],
                        "ip":       worker['fs-host']['addr'],
                        "dns":      worker['fs-host']['name'],
                        "cssClass": "bg-white"
                    },
                    {   "name":     "Exporter",
                        "protocol": "binary",
                        "port":     worker['exporter-port'],
                        "ip":       worker['exporter-host']['addr'],
                        "dns":      worker['exporter-host']['name'],
                        "cssClass": "bg-white"
                    },
                    {   "name":     "Ingest",
                        "protocol": "binary",
                        "port":     worker['loader-port'],
                        "ip":       worker['loader-host']['addr'],
                        "dns":      worker['loader-host']['name'],
                        "cssClass": "bg-white"
                    },
                    {   "name":     "Ingest",
                        "protocol": "http",
                        "port":     worker['http-loader-port'],
                        "ip":       worker['http-loader-host']['addr'],
                        "dns":      worker['http-loader-host']['name'],
                        "cssClass": "bg-info"
                    },
                    {   "name":     "Qserv Worker Manager",
                        "protocol": "http",
                        "port":     worker['qserv-worker']['port'],
                        "ip":       worker['qserv-worker']['host']['addr'],
                        "dns":      worker['qserv-worker']['host']['name'],
                        "cssClass": "bg-info"
                    }
                ];
                html += `
<tr>
  <th rowspan="${service.length + 1}" style="text-align:left; vertical-align: top;" scope="row">${worker['name']}</th>
  <td rowspan="${service.length + 1}" style="text-align:left; vertical-align: top;" ${workerEnabledCssClass}><pre>${worker['is-enabled'] ? 'yes' : 'no'}</pre></td>
  <td rowspan="${service.length + 1}" style="text-align:left; vertical-align: top;" ${workerReadOnlyCssClass}><pre>${worker['is-read-only'] ? 'yes' : 'no'}</pre></td>
</tr>`;
                html += _.reduce(service, function(html, svc) {
                    return html += `
<tr>
  <td><pre>${svc.name}</pre></td>
  <td class="${svc.cssClass}"><pre>${svc.protocol}</pre></td>
  <td><pre>${svc.port}</pre></td>
  <td style="opacity: 0.6"><pre>${svc.ip}</pre></td>
  <td style="opacity: 0.6"><pre>${svc.dns}</pre></td>
</tr>`;

                }, '');
            }
            this._table().children('tbody').html(html);
        }
    }
    return ReplicationConfigWorkers;
});

