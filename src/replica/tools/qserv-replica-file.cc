/*
 * LSST Data Management System
 *
 * This product includes software developed by the
 * LSST Project (http://www.lsst.org/).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the LSST License Statement and
 * the GNU General Public License along with this program.  If not,
 * see <http://www.lsstcorp.org/LegalNotices/>.
 */

// System headers
#include <iostream>
#include <stdexcept>

// Qserv headers
#include "replica/apps/ApplicationColl.h"
#include "replica/apps/TestAwsS3App.h"
#include "replica/apps/FileExportApp.h"
#include "replica/apps/FileIngestApp.h"
#include "replica/apps/FileReadApp.h"
#include "replica/apps/FileServerApp.h"

using namespace std;
using namespace lsst::qserv::replica;

namespace {

ApplicationColl getAppColl() {
    ApplicationColl coll;
    coll.add<TestAwsS3App>("AWS-S3");
    coll.add<FileExportApp>("EXPORT");
    coll.add<FileIngestApp>("INGEST");
    coll.add<FileReadApp>("READ");
    coll.add<FileServerApp>("SERVER");
    return coll;
}
}  // namespace

int main(int argc, char* argv[]) {
    try {
        return getAppColl().run(argc, argv);
    } catch (exception const& ex) {
        cerr << argv[0] << ": the application '" << argv[1] << "' failed, exception: " << ex.what() << endl;
        return 1;
    }
}
