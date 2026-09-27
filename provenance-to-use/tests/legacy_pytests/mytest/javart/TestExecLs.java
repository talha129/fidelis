import java.io.*;  
public class TestExecLs {  
    public static void main(String[] args) {  
        try {  
            Process p = Runtime.getRuntime().exec("/bin/echo \"this is a very long parameter that I want to see it all, so it must be displayed until the end with an exclaimation mark: !\"");  
            BufferedReader in = new BufferedReader(  
                                new InputStreamReader(p.getInputStream()));  
            String line = null;  
            while ((line = in.readLine()) != null) {  
                System.out.println(line);  
            }  
        } catch (IOException e) {  
            e.printStackTrace();  
        }  
    }  
}
