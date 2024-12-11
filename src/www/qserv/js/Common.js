define([
    'modules/sql-formatter.min',
    'underscore'],

function(sqlFormatter,
         _) {

    class Common {
        static RestAPIVersion = 40;
        static query2text(query, expanded) {
            if (expanded) {
                if (query.length > Common._max_expanded_length) {
                    return sqlFormatter.format(
                        "************* ATTENTION **************;" +
                        "*Query has been truncated at " + Common._max_expanded_length + " bytes since it is too long*;" +
                        "*Click the download button to see the full text of the query*;" +
                        "********************************;" +
                        ";" + 
                        query.substring(0, Common._max_expanded_length) + "...",
                        Common._sqlFormatterConfig);
                } else {
                    return sqlFormatter.format(query, Common._sqlFormatterConfig);
                }
            } else if (query.length > Common._max_compact_length) {
                return query.substring(0, Common._max_compact_length) + "...";
            } else {
                return query;
            }
        }
        static _sqlFormatterConfig = {"language":"mysql", "uppercase:":true, "indent":"  "};
        static _max_compact_length = 104;
        static _max_expanded_length = 4096;
        static _ivals = [
            {value:   2, name:  '2 sec'},
            {value:   5, name:  '5 sec'},
            {value:  10, name: '10 sec'},
            {value:  20, name: '20 sec'},
            {value:  30, name: '30 sec'},
            {value:  60, name:  '1 min'},
            {value: 120, name:  '2 min'},
            {value: 300, name:  '5 min'},
            {value: 600, name: '10 min'}
        ];
        static html_update_ival(id, default_ival = 30, ivals = undefined) {
            return `
<label for="${id}"><i class="bi bi-arrow-repeat"></i>&nbsp;interval:</label>
<select id="${id}" class="form-control form-control-selector">` + _.reduce(ivals || Common._ivals, function (html, ival) { return html + `
  <option value="${ival.value}" ${ival.value == default_ival ? 'selected' : ''}>${ival.name}</option>`;
            }, '') + `
</select>`;
        }
        static KB = 1000;
        static MB = 1000 * 1000;
        static GB = 1000 * 1000 * 1000;
        static format_data_rate(v) {
            if (v == 0) return v + "";  // as string
            else if (v < Common.KB * 10) return v.toFixed(0);
            else if (v < Common.MB * 10) return (v / Common.KB).toFixed(0) + " KB";
            else if (v < Common.GB * 10) return (v / Common.MB).toFixed(0) + " MB";
            else                         return (v / Common.GB).toFixed(0) + " GB";
        }
    }
    return Common;
});
